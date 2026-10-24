// SPDX-License-Identifier: GPL-2.0-only
/*
 * ADXL345 I2C driver, built out of tree.
 *
 * Kbuild compiles this file with -std=gnu11. That is the newest C dialect
 * this kernel allows a module to use; the build system sets it and a module
 * must not override it. _Static_assert below is C11.
 *
 * This is not the in-tree driver. Those ids stay untouched:
 *   input adxl34x_i2c  id "adxl34x"   samples only from its IRQ handler
 *   iio   adxl345_i2c  id "adxl345"
 * This module binds only "adxl345_custom".
 *
 * The Adafruit breakout is in I2C mode at 0x53 (CS pulled high, SDO low).
 * Its interrupt pin, silkscreened INT, is wired to Pi pin 11 (GPIO17).
 * DATA_READY is routed to that pin. The pin stays high until the data
 * registers are read, so the GPIO interrupt is level-high. A rising-edge
 * request is the fallback if the controller rejects a level trigger.
 * int_line=0 tries INT1, then INT2. irq_gpio=-1 keeps the old polled read.
 *
 * Full resolution, +/-2 g, 100 Hz. The scale stays 256 LSB/g (3.9 mg/LSB).
 * A burst starts at DATAX0 (0x32). I2C does not use the SPI multi-byte bit.
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/timekeeping.h>
#include <linux/unaligned.h>
#include <linux/wait.h>

#include "adxl345/custom_mod_abi.h"

#define ADXL345_REG_DEVID		0x00
#define ADXL345_REG_OFSX		0x1E
#define ADXL345_REG_BW_RATE		0x2C
#define ADXL345_REG_POWER_CTL		0x2D
#define ADXL345_REG_INT_ENABLE		0x2E
#define ADXL345_REG_INT_MAP		0x2F
#define ADXL345_REG_INT_SOURCE		0x30
#define ADXL345_REG_DATA_FORMAT		0x31
#define ADXL345_REG_DATAX0		0x32
#define ADXL345_REG_FIFO_CTL		0x38

#define ADXL345_DEVID			0xE5
#define ADXL345_BW_RATE_100HZ		0x0A
#define ADXL345_POWER_MEASURE		0x08
#define ADXL345_DATA_FORMAT_FULL_RES	0x08
#define ADXL345_READ_TIMEOUT_MS		200

_Static_assert(ADXL345_REG_DATAX0 == 0x32, "burst starts at DATAX0");
_Static_assert(ADXL345_DEVID == 0xE5, "ADXL345 device id");
_Static_assert(ADXL345_CUSTOM_LSB_PER_G == 256, "full-resolution scale");
_Static_assert(ADXL345_DATA_FORMAT_FULL_RES == 0x08, "full-res +/-2 g");
_Static_assert(ADXL345_CUSTOM_INT_DATA_READY == 0x80, "DATA_READY is bit 7");
_Static_assert(ADXL345_CUSTOM_INT_OVERRUN == 0x01, "OVERRUN is bit 0");
_Static_assert(ADXL345_CUSTOM_INT_ENABLE_BITS == 0x81, "data-ready and overrun");
_Static_assert(ADXL345_CUSTOM_IRQ_TYPE_LEVEL_HIGH == 4, "level-high trigger");

static int bus_num = 1;
static unsigned short addr = 0x53;
static bool autoprobe = true;
static int irq_gpio = 17;
static int int_line;

module_param(bus_num, int, 0444);
MODULE_PARM_DESC(bus_num, "I2C adapter number (default 1)");
module_param(addr, ushort, 0444);
MODULE_PARM_DESC(addr, "7-bit I2C address (default 0x53)");
module_param(autoprobe, bool, 0444);
MODULE_PARM_DESC(autoprobe, "Create the i2c client during init (default true)");
module_param(irq_gpio, int, 0444);
MODULE_PARM_DESC(irq_gpio, "BCM GPIO for the INT wire, -1 to poll (default 17, Pi pin 11)");
module_param(int_line, int, 0444);
MODULE_PARM_DESC(int_line, "1 = INT1, 2 = INT2, 0 = try INT1 then INT2 (default 0)");

struct adxl345_dev {
	struct i2c_client *client;
	struct miscdevice misc;
	struct mutex lock;
	wait_queue_head_t wq;
	struct gpio_desc *int_gpio;
	struct gpio_device *gdev;
	struct adxl345_custom_sample latest;
	u32 sequence;
	u32 read_seq;
	u32 irq_count;
	u32 overruns;
	u32 dropped;
	u32 spurious;
	u32 i2c_errors;
	int irq;
	int int_line;
	bool misc_ready;
	bool irq_requested;
	bool irq_on;
	bool level_trigger;
};

static struct i2c_client *adxl345_custom_client;

/* SMBus helpers copy into a DMA-safe buffer. The Pi adapter may DMA. */
static int adxl345_read_regs(struct i2c_client *client, u8 reg,
			     void *dst, u16 len)
{
	int ret;

	if (len == 0 || len > I2C_SMBUS_BLOCK_MAX)
		return -EINVAL;

	if (len == 1) {
		ret = i2c_smbus_read_byte_data(client, reg);
		if (ret < 0)
			return ret;
		*(u8 *)dst = (u8)ret;
		return 0;
	}

	ret = i2c_smbus_read_i2c_block_data(client, reg, (u8)len, dst);
	if (ret < 0)
		return ret;
	return ret == len ? 0 : -EIO;
}

static int adxl345_write_reg(struct i2c_client *client, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(client, reg, val);
}

static void adxl345_decode_axes(const u8 raw[6], struct adxl345_custom_sample *out)
{
	out->x = (adxl345_s16)get_unaligned_le16(raw);
	out->y = (adxl345_s16)get_unaligned_le16(raw + 2);
	out->z = (adxl345_s16)get_unaligned_le16(raw + 4);
	out->lsb_per_g = ADXL345_CUSTOM_LSB_PER_G;
}

static int adxl345_read_axes(struct adxl345_dev *dev, struct adxl345_custom_sample *out)
{
	u8 raw[6];
	int ret;

	ret = adxl345_read_regs(dev->client, ADXL345_REG_DATAX0, raw, sizeof(raw));
	if (ret)
		return ret;
	adxl345_decode_axes(raw, out);
	return 0;
}

static void adxl345_publish(struct adxl345_dev *dev, struct adxl345_custom_sample *sample)
{
	sample->sequence = adxl345_custom_note_publish(&dev->sequence, dev->read_seq,
						      &dev->dropped);
	sample->overruns = dev->overruns;
	sample->timestamp_ns = ktime_get_ns();
	dev->latest = *sample;
	WRITE_ONCE(dev->sequence, dev->sequence);
}

static void adxl345_standby(void *data)
{
	struct i2c_client *client = data;
	struct adxl345_dev *dev = i2c_get_clientdata(client);

	if (dev)
		mutex_lock(&dev->lock);
	adxl345_write_reg(client, ADXL345_REG_INT_ENABLE, 0);
	adxl345_write_reg(client, ADXL345_REG_POWER_CTL, 0);
	if (dev)
		mutex_unlock(&dev->lock);
}

static int adxl345_configure(struct i2c_client *client)
{
	u8 id = 0;
	u8 format = 0;
	u8 rate = 0;
	u8 zeroes[3] = { 0, 0, 0 };
	int ret;

	ret = adxl345_write_reg(client, ADXL345_REG_POWER_CTL, 0);
	if (ret)
		return dev_err_probe(&client->dev, ret, "standby failed\n");

	ret = adxl345_read_regs(client, ADXL345_REG_DEVID, &id, 1);
	if (ret)
		return dev_err_probe(&client->dev, ret, "DEVID read failed\n");
	if (id != ADXL345_DEVID)
		return dev_err_probe(&client->dev, -ENODEV,
				     "DEVID 0x%02x, expected 0x%02x\n",
				     id, ADXL345_DEVID);

	ret = i2c_smbus_write_i2c_block_data(client, ADXL345_REG_OFSX,
					     (u8)sizeof(zeroes), zeroes);
	if (ret)
		return dev_err_probe(&client->dev, ret, "clearing offsets failed\n");

	ret = adxl345_write_reg(client, ADXL345_REG_BW_RATE, ADXL345_BW_RATE_100HZ);
	if (ret)
		return dev_err_probe(&client->dev, ret, "BW_RATE write failed\n");

	ret = adxl345_write_reg(client, ADXL345_REG_DATA_FORMAT,
				ADXL345_DATA_FORMAT_FULL_RES);
	if (ret)
		return dev_err_probe(&client->dev, ret, "DATA_FORMAT write failed\n");

	ret = adxl345_write_reg(client, ADXL345_REG_INT_ENABLE, 0);
	if (ret)
		return dev_err_probe(&client->dev, ret, "INT_ENABLE write failed\n");

	ret = adxl345_write_reg(client, ADXL345_REG_INT_MAP, 0);
	if (ret)
		return dev_err_probe(&client->dev, ret, "INT_MAP write failed\n");

	ret = adxl345_write_reg(client, ADXL345_REG_FIFO_CTL, 0);
	if (ret)
		return dev_err_probe(&client->dev, ret, "FIFO_CTL write failed\n");

	ret = adxl345_write_reg(client, ADXL345_REG_POWER_CTL, ADXL345_POWER_MEASURE);
	if (ret)
		return dev_err_probe(&client->dev, ret, "measure enable failed\n");

	ret = devm_add_action_or_reset(&client->dev, adxl345_standby, client);
	if (ret)
		return ret;

	ret = adxl345_read_regs(client, ADXL345_REG_DATA_FORMAT, &format, 1);
	if (ret)
		return ret;
	ret = adxl345_read_regs(client, ADXL345_REG_BW_RATE, &rate, 1);
	if (ret)
		return ret;

	dev_info(&client->dev,
		 "measuring, data_format 0x%02x, bw_rate 0x%02x, %u LSB/g\n",
		 format, rate, ADXL345_CUSTOM_LSB_PER_G);
	return 0;
}

static int adxl345_match_bcm_gpio(struct gpio_chip *gc, const void *data)
{
	struct device *parent = gc->parent;

	(void)data;
	if (!parent || !parent->of_node)
		return 0;
	return of_device_is_compatible(parent->of_node, "brcm,bcm2835-gpio") ||
	       of_device_is_compatible(parent->of_node, "brcm,bcm2711-gpio");
}

static struct gpio_device *adxl345_find_gpio_device(void)
{
	static const char * const labels[] = {
		"pinctrl-bcm2835",
		"pinctrl-bcm2710",
		"pinctrl-bcm2711",
	};
	unsigned int i;
	struct gpio_device *gdev;

	for (i = 0; i < ARRAY_SIZE(labels); i++) {
		gdev = gpio_device_find_by_label(labels[i]);
		if (gdev)
			return gdev;
	}
	return gpio_device_find(NULL, adxl345_match_bcm_gpio);
}

static void adxl345_irq_cleanup(void *data)
{
	struct adxl345_dev *dev = data;

	mutex_lock(&dev->lock);
	dev->irq_on = false;
	WRITE_ONCE(dev->irq_on, false);
	adxl345_write_reg(dev->client, ADXL345_REG_INT_ENABLE, 0);
	mutex_unlock(&dev->lock);
	wake_up_interruptible(&dev->wq);

	if (dev->irq_requested) {
		free_irq(dev->irq, dev);
		dev->irq_requested = false;
	}
	if (dev->int_gpio) {
		gpiochip_free_own_desc(dev->int_gpio);
		dev->int_gpio = NULL;
	}
	if (dev->gdev) {
		gpio_device_put(dev->gdev);
		dev->gdev = NULL;
	}
}

static irqreturn_t adxl345_irq_thread(int irq, void *data)
{
	struct adxl345_dev *dev = data;
	struct adxl345_custom_sample sample;
	u8 source = 0;
	int ret;

	(void)irq;
	mutex_lock(&dev->lock);
	if (!dev->irq_on) {
		mutex_unlock(&dev->lock);
		return IRQ_HANDLED;
	}

	ret = adxl345_read_regs(dev->client, ADXL345_REG_INT_SOURCE, &source, 1);
	if (ret) {
		dev->i2c_errors++;
		dev_err_ratelimited(&dev->client->dev, "INT_SOURCE read failed: %d\n", ret);
		mutex_unlock(&dev->lock);
		return IRQ_HANDLED;
	}

	if (source & ADXL345_CUSTOM_INT_OVERRUN)
		dev->overruns++;
	if (!(source & ADXL345_CUSTOM_INT_DATA_READY) &&
	    !(source & ADXL345_CUSTOM_INT_OVERRUN))
		dev->spurious++;

	ret = adxl345_read_axes(dev, &sample);
	if (ret) {
		dev->i2c_errors++;
		dev_err_ratelimited(&dev->client->dev, "sample read failed: %d\n", ret);
		mutex_unlock(&dev->lock);
		return IRQ_HANDLED;
	}

	/* Reading DATAX0..DATAZ1 clears DATA_READY and OVERRUN. */
	if (source & ADXL345_CUSTOM_INT_DATA_READY) {
		dev->irq_count++;
		adxl345_publish(dev, &sample);
	}
	mutex_unlock(&dev->lock);
	wake_up_interruptible(&dev->wq);
	return IRQ_HANDLED;
}

static int adxl345_map_line(struct adxl345_dev *dev, int line)
{
	u8 map = (u8)adxl345_custom_int_map(line);
	u8 junk = 0;
	struct adxl345_custom_sample toss;
	int ret;

	ret = adxl345_write_reg(dev->client, ADXL345_REG_INT_ENABLE, 0);
	if (ret)
		return ret;
	ret = adxl345_write_reg(dev->client, ADXL345_REG_INT_MAP, map);
	if (ret)
		return ret;
	/* INT_SOURCE clears the latched bits. The data burst clears DATA_READY. */
	ret = adxl345_read_regs(dev->client, ADXL345_REG_INT_SOURCE, &junk, 1);
	if (ret)
		return ret;
	ret = adxl345_read_axes(dev, &toss);
	if (ret)
		return ret;
	dev->int_line = line;
	return adxl345_write_reg(dev->client, ADXL345_REG_INT_ENABLE,
				 ADXL345_CUSTOM_INT_ENABLE_BITS);
}

static int adxl345_wait_data_ready(struct adxl345_dev *dev)
{
	u32 seen = READ_ONCE(dev->sequence);

	wait_event_timeout(dev->wq, READ_ONCE(dev->sequence) != seen,
			   msecs_to_jiffies(80));
	return READ_ONCE(dev->sequence) == seen ? -ETIMEDOUT : 0;
}

static int adxl345_arm_line(struct adxl345_dev *dev, int line)
{
	int ret;

	mutex_lock(&dev->lock);
	ret = adxl345_map_line(dev, line);
	mutex_unlock(&dev->lock);
	if (ret)
		return ret;
	return adxl345_wait_data_ready(dev);
}

static int adxl345_claim_gpio(struct adxl345_dev *dev)
{
	struct gpio_chip *gc;
	struct gpio_desc *desc;
	unsigned long flags = IRQF_ONESHOT | IRQF_TRIGGER_HIGH;
	int ret;

	dev->gdev = adxl345_find_gpio_device();
	if (!dev->gdev)
		return dev_err_probe(&dev->client->dev, -ENODEV,
				     "SoC GPIO controller not found\n");

	gc = gpio_device_get_chip(dev->gdev);
	if (!gc || irq_gpio < 0 || (unsigned int)irq_gpio >= gc->ngpio) {
		gpio_device_put(dev->gdev);
		dev->gdev = NULL;
		return dev_err_probe(&dev->client->dev, -EINVAL,
				     "GPIO %d is outside the controller\n", irq_gpio);
	}

	desc = gpiochip_request_own_desc(gc, (unsigned int)irq_gpio, "adxl345-int",
					 (enum gpio_lookup_flags)(GPIO_PERSISTENT | GPIO_PULL_DOWN),
					 GPIOD_IN);
	if (IS_ERR(desc)) {
		ret = PTR_ERR(desc);
		gpio_device_put(dev->gdev);
		dev->gdev = NULL;
		return dev_err_probe(&dev->client->dev, ret,
				     "requesting GPIO %d failed\n", irq_gpio);
	}
	dev->int_gpio = desc;

	ret = devm_add_action_or_reset(&dev->client->dev, adxl345_irq_cleanup, dev);
	if (ret)
		return ret;

	dev->irq = gpiod_to_irq(desc);
	if (dev->irq < 0)
		return dev_err_probe(&dev->client->dev, dev->irq,
				     "GPIO %d has no IRQ\n", irq_gpio);

	ret = request_threaded_irq(dev->irq, NULL, adxl345_irq_thread, flags,
				   "adxl345_custom", dev);
	if (ret) {
		flags = IRQF_ONESHOT | IRQF_TRIGGER_RISING;
		ret = request_threaded_irq(dev->irq, NULL, adxl345_irq_thread, flags,
					   "adxl345_custom", dev);
		if (ret)
			return dev_err_probe(&dev->client->dev, ret,
					     "requesting GPIO %d IRQ failed\n", irq_gpio);
		dev->level_trigger = false;
	} else {
		dev->level_trigger = true;
	}
	dev->irq_requested = true;
	return 0;
}

static int adxl345_start_irq(struct adxl345_dev *dev)
{
	int first = int_line == 2 ? 2 : 1;
	int second = first == 1 ? 2 : 1;
	int ret;

	ret = adxl345_claim_gpio(dev);
	if (ret)
		return ret;

	dev->irq_on = true;
	WRITE_ONCE(dev->irq_on, true);
	ret = adxl345_arm_line(dev, first);
	if (ret && int_line == 0) {
		dev_info(&dev->client->dev,
			 "no data-ready on INT%d, trying INT%d\n", first, second);
		ret = adxl345_arm_line(dev, second);
	}
	if (ret) {
		dev_warn(&dev->client->dev,
			 "GPIO %d produced no data-ready interrupt; polling\n", irq_gpio);
		mutex_lock(&dev->lock);
		dev->irq_on = false;
		WRITE_ONCE(dev->irq_on, false);
		adxl345_write_reg(dev->client, ADXL345_REG_INT_ENABLE, 0);
		mutex_unlock(&dev->lock);
		if (dev->irq_requested) {
			free_irq(dev->irq, dev);
			dev->irq_requested = false;
		}
		return 0;
	}

	dev_info(&dev->client->dev,
		 "data-ready on GPIO %d via INT%d (%s)\n",
		 irq_gpio, dev->int_line,
		 dev->level_trigger ? "level-high" : "rising-edge");
	return 0;
}

static ssize_t adxl345_custom_read(struct file *file, char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct miscdevice *misc = file->private_data;
	struct adxl345_dev *dev = container_of(misc, struct adxl345_dev, misc);
	struct adxl345_custom_sample sample;
	u32 seen;
	int ret;

	if (count < sizeof(sample))
		return -EINVAL;

	if (!READ_ONCE(dev->irq_on)) {
		mutex_lock(&dev->lock);
		ret = adxl345_read_axes(dev, &sample);
		if (!ret) {
			adxl345_publish(dev, &sample);
			dev->read_seq = sample.sequence;
			WRITE_ONCE(dev->read_seq, dev->read_seq);
		}
		mutex_unlock(&dev->lock);
		if (ret)
			return ret;
		if (copy_to_user(buf, &sample, sizeof(sample)))
			return -EFAULT;
		*ppos = 0;
		return sizeof(sample);
	}

	seen = READ_ONCE(dev->read_seq);
	if (file->f_flags & O_NONBLOCK) {
		if (READ_ONCE(dev->sequence) == seen)
			return -EAGAIN;
	} else {
		ret = wait_event_interruptible_timeout(dev->wq,
			READ_ONCE(dev->sequence) != seen || !READ_ONCE(dev->irq_on),
			msecs_to_jiffies(ADXL345_READ_TIMEOUT_MS));
		if (ret < 0)
			return ret;
		if (READ_ONCE(dev->sequence) == seen)
			return -ETIMEDOUT;
	}

	mutex_lock(&dev->lock);
	sample = dev->latest;
	dev->read_seq = sample.sequence;
	WRITE_ONCE(dev->read_seq, dev->read_seq);
	mutex_unlock(&dev->lock);

	if (copy_to_user(buf, &sample, sizeof(sample)))
		return -EFAULT;
	*ppos = 0;
	return sizeof(sample);
}

static __poll_t adxl345_poll(struct file *file, poll_table *wait)
{
	struct miscdevice *misc = file->private_data;
	struct adxl345_dev *dev = container_of(misc, struct adxl345_dev, misc);

	poll_wait(file, &dev->wq, wait);
	if (!READ_ONCE(dev->irq_on))
		return EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(dev->sequence) != READ_ONCE(dev->read_seq))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static const struct file_operations adxl345_custom_fops = {
	.owner = THIS_MODULE,
	.read = adxl345_custom_read,
	.poll = adxl345_poll,
	.llseek = noop_llseek,
};

static struct adxl345_dev *adxl345_dev_from(struct device *kdev)
{
	return i2c_get_clientdata(to_i2c_client(kdev));
}

static ssize_t adxl345_axis_show(struct device *kdev, char *buf, int axis)
{
	struct adxl345_dev *dev = adxl345_dev_from(kdev);
	struct adxl345_custom_sample sample;
	int ret = 0;
	s16 value;

	if (!dev)
		return -ENODEV;

	mutex_lock(&dev->lock);
	if (dev->irq_on) {
		if (dev->sequence == 0)
			ret = -ENODATA;
		else
			sample = dev->latest;
	} else {
		ret = adxl345_read_axes(dev, &sample);
	}
	mutex_unlock(&dev->lock);
	if (ret)
		return ret;

	value = axis == 0 ? sample.x : axis == 1 ? sample.y : sample.z;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t x_raw_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	(void)attr;
	return adxl345_axis_show(kdev, buf, 0);
}
static DEVICE_ATTR_RO(x_raw);

static ssize_t y_raw_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	(void)attr;
	return adxl345_axis_show(kdev, buf, 1);
}
static DEVICE_ATTR_RO(y_raw);

static ssize_t z_raw_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	(void)attr;
	return adxl345_axis_show(kdev, buf, 2);
}
static DEVICE_ATTR_RO(z_raw);

static ssize_t lsb_per_g_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	(void)kdev;
	(void)attr;
	return sysfs_emit(buf, "%u\n", ADXL345_CUSTOM_LSB_PER_G);
}
static DEVICE_ATTR_RO(lsb_per_g);

static ssize_t devid_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	struct adxl345_dev *dev = adxl345_dev_from(kdev);
	u8 id = 0;
	int ret;

	(void)attr;
	if (!dev)
		return -ENODEV;

	mutex_lock(&dev->lock);
	ret = adxl345_read_regs(dev->client, ADXL345_REG_DEVID, &id, 1);
	mutex_unlock(&dev->lock);
	if (ret)
		return ret;
	return sysfs_emit(buf, "0x%02x\n", id);
}
static DEVICE_ATTR_RO(devid);

static ssize_t mode_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	struct adxl345_dev *dev = adxl345_dev_from(kdev);

	(void)attr;
	if (!dev)
		return -ENODEV;
	return sysfs_emit(buf, "%s\n", READ_ONCE(dev->irq_on) ? "irq" : "poll");
}
static DEVICE_ATTR_RO(mode);

static ssize_t irq_gpio_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	(void)kdev;
	(void)attr;
	return sysfs_emit(buf, "%d\n", irq_gpio);
}
static DEVICE_ATTR_RO(irq_gpio);

static ssize_t int_line_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	struct adxl345_dev *dev = adxl345_dev_from(kdev);

	(void)attr;
	if (!dev)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", READ_ONCE(dev->irq_on) ? dev->int_line : 0);
}
static DEVICE_ATTR_RO(int_line);

static ssize_t trigger_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	struct adxl345_dev *dev = adxl345_dev_from(kdev);

	(void)attr;
	if (!dev)
		return -ENODEV;
	if (!READ_ONCE(dev->irq_on))
		return sysfs_emit(buf, "none\n");
	return sysfs_emit(buf, "%s\n", dev->level_trigger ? "level-high" : "rising");
}
static DEVICE_ATTR_RO(trigger);

#define ADXL345_COUNT_SHOW(name, member)					\
static ssize_t name##_show(struct device *kdev,				\
			   struct device_attribute *attr, char *buf)		\
{										\
	struct adxl345_dev *dev = adxl345_dev_from(kdev);			\
	(void)attr;								\
	if (!dev)								\
		return -ENODEV;							\
	return sysfs_emit(buf, "%u\n", READ_ONCE(dev->member));			\
}										\
static DEVICE_ATTR_RO(name)

ADXL345_COUNT_SHOW(irq_count, irq_count);
ADXL345_COUNT_SHOW(overruns, overruns);
ADXL345_COUNT_SHOW(dropped, dropped);
ADXL345_COUNT_SHOW(spurious, spurious);
ADXL345_COUNT_SHOW(i2c_errors, i2c_errors);
ADXL345_COUNT_SHOW(sequence, sequence);

static struct attribute *adxl345_attrs[] = {
	&dev_attr_x_raw.attr,
	&dev_attr_y_raw.attr,
	&dev_attr_z_raw.attr,
	&dev_attr_lsb_per_g.attr,
	&dev_attr_devid.attr,
	&dev_attr_mode.attr,
	&dev_attr_irq_gpio.attr,
	&dev_attr_int_line.attr,
	&dev_attr_trigger.attr,
	&dev_attr_irq_count.attr,
	&dev_attr_overruns.attr,
	&dev_attr_dropped.attr,
	&dev_attr_spurious.attr,
	&dev_attr_i2c_errors.attr,
	&dev_attr_sequence.attr,
	NULL,
};
ATTRIBUTE_GROUPS(adxl345);

static int adxl345_custom_probe(struct i2c_client *client)
{
	struct adxl345_dev *dev;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(&client->dev, -EOPNOTSUPP,
				     "adapter does not support I2C transfers\n");
	if (int_line < 0 || int_line > 2)
		return -EINVAL;

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->client = client;
	mutex_init(&dev->lock);
	init_waitqueue_head(&dev->wq);
	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name = "adxl345_custom";
	dev->misc.fops = &adxl345_custom_fops;
	dev->misc.parent = &client->dev;
	dev->misc.mode = 0644;

	ret = adxl345_configure(client);
	if (ret)
		return ret;

	if (irq_gpio >= 0) {
		ret = adxl345_start_irq(dev);
		if (ret)
			return ret;
	}

	i2c_set_clientdata(client, dev);

	ret = misc_register(&dev->misc);
	if (ret)
		return dev_err_probe(&client->dev, ret, "misc_register failed\n");
	dev->misc_ready = true;

	dev_info(&client->dev, "bound as /dev/adxl345_custom (%s)\n",
		 dev->irq_on ? "irq" : "poll");
	return 0;
}

static void adxl345_custom_remove(struct i2c_client *client)
{
	struct adxl345_dev *dev = i2c_get_clientdata(client);

	if (dev && dev->misc_ready)
		misc_deregister(&dev->misc);
}

static const struct i2c_device_id adxl345_custom_id[] = {
	{ "adxl345_custom", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, adxl345_custom_id);

static struct i2c_driver adxl345_custom_driver = {
	.driver = {
		.name = "adxl345_custom",
		.dev_groups = adxl345_groups,
	},
	.probe = adxl345_custom_probe,
	.remove = adxl345_custom_remove,
	.id_table = adxl345_custom_id,
};

static int __init adxl345_custom_init(void)
{
	struct i2c_adapter *adapter;
	struct i2c_board_info info = {
		I2C_BOARD_INFO("adxl345_custom", addr),
	};
	int ret;

	if (bus_num < 0 || addr > 0x7f || int_line < 0 || int_line > 2)
		return -EINVAL;

	ret = i2c_add_driver(&adxl345_custom_driver);
	if (ret)
		return ret;
	if (!autoprobe)
		return 0;

	adapter = i2c_get_adapter(bus_num);
	if (!adapter) {
		i2c_del_driver(&adxl345_custom_driver);
		return -ENODEV;
	}

	adxl345_custom_client = i2c_new_client_device(adapter, &info);
	i2c_put_adapter(adapter);
	if (IS_ERR(adxl345_custom_client)) {
		ret = PTR_ERR(adxl345_custom_client);
		adxl345_custom_client = NULL;
		i2c_del_driver(&adxl345_custom_driver);
		return ret;
	}
	return 0;
}

static void __exit adxl345_custom_exit(void)
{
	if (adxl345_custom_client)
		i2c_unregister_device(adxl345_custom_client);
	i2c_del_driver(&adxl345_custom_driver);
}

module_init(adxl345_custom_init);
module_exit(adxl345_custom_exit);

MODULE_DESCRIPTION("ADXL345 I2C driver with data-ready IRQ (out of tree, id adxl345_custom)");
MODULE_LICENSE("GPL v2");
