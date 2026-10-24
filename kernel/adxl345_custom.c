// SPDX-License-Identifier: GPL-2.0-only
/*
 * Polled ADXL345 I2C driver, built out of tree.
 *
 * Kbuild compiles this file with -std=gnu11. That is the newest C dialect
 * this kernel allows a module to use; the build system sets it and a module
 * must not override it. _Static_assert below is C11.
 *
 * This is not the in-tree driver. Those ids stay untouched:
 *   input adxl34x_i2c  id "adxl34x"   needs an IRQ line
 *   iio   adxl345_i2c  id "adxl345"   polled sysfs when no IRQ is described
 * This module binds only "adxl345_custom".
 *
 * The Adafruit breakout here has four wires: 3V3, GND, SDA, SCL. CS is pulled
 * high on the board, so the part is in I2C mode at 0x53. INT1 and INT2 are
 * not connected. Samples are polled.
 *
 * Full resolution, +/-2 g, 100 Hz. The scale stays 256 LSB/g (3.9 mg/LSB).
 * A burst starts at DATAX0 (0x32). I2C does not use the SPI multi-byte bit.
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include "adxl345/custom_mod_abi.h"

#define ADXL345_REG_DEVID		0x00
#define ADXL345_REG_OFSX		0x1E
#define ADXL345_REG_BW_RATE		0x2C
#define ADXL345_REG_POWER_CTL		0x2D
#define ADXL345_REG_INT_ENABLE		0x2E
#define ADXL345_REG_DATA_FORMAT		0x31
#define ADXL345_REG_DATAX0		0x32
#define ADXL345_REG_FIFO_CTL		0x38

#define ADXL345_DEVID			0xE5
#define ADXL345_BW_RATE_100HZ		0x0A
#define ADXL345_POWER_MEASURE		0x08
#define ADXL345_DATA_FORMAT_FULL_RES	0x08

_Static_assert(ADXL345_REG_DATAX0 == 0x32, "burst starts at DATAX0");
_Static_assert(ADXL345_DEVID == 0xE5, "ADXL345 device id");
_Static_assert(ADXL345_CUSTOM_LSB_PER_G == 256, "full-resolution scale");
_Static_assert(ADXL345_DATA_FORMAT_FULL_RES == 0x08, "full-res +/-2 g");

static int bus_num = 1;
static unsigned short addr = 0x53;
static bool autoprobe = true;

module_param(bus_num, int, 0444);
MODULE_PARM_DESC(bus_num, "I2C adapter number (default 1)");
module_param(addr, ushort, 0444);
MODULE_PARM_DESC(addr, "7-bit I2C address (default 0x53)");
module_param(autoprobe, bool, 0444);
MODULE_PARM_DESC(autoprobe, "Create the i2c client during init (default true)");

struct adxl345_dev {
	struct i2c_client *client;
	struct miscdevice misc;
	struct mutex lock;
	bool misc_ready;
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

static int adxl345_read_sample(struct adxl345_dev *dev,
			       struct adxl345_custom_sample *out)
{
	u8 raw[6];
	int ret;

	ret = adxl345_read_regs(dev->client, ADXL345_REG_DATAX0, raw, sizeof(raw));
	if (ret)
		return ret;

	out->x = (adxl345_s16)get_unaligned_le16(raw);
	out->y = (adxl345_s16)get_unaligned_le16(raw + 2);
	out->z = (adxl345_s16)get_unaligned_le16(raw + 4);
	out->lsb_per_g = ADXL345_CUSTOM_LSB_PER_G;
	return 0;
}

static void adxl345_standby(void *data)
{
	struct i2c_client *client = data;
	struct adxl345_dev *dev = i2c_get_clientdata(client);

	if (dev)
		mutex_lock(&dev->lock);
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

	/* OFSX, OFSY, OFSZ. The part auto-increments the register pointer. */
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

static ssize_t adxl345_custom_read(struct file *file, char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct miscdevice *misc = file->private_data;
	struct adxl345_dev *dev = container_of(misc, struct adxl345_dev, misc);
	struct adxl345_custom_sample sample;
	int ret;

	if (count < sizeof(sample))
		return -EINVAL;

	mutex_lock(&dev->lock);
	ret = adxl345_read_sample(dev, &sample);
	mutex_unlock(&dev->lock);
	if (ret)
		return ret;
	if (copy_to_user(buf, &sample, sizeof(sample)))
		return -EFAULT;

	/* Each read is one new sample. The offset is not a file position. */
	*ppos = 0;
	return sizeof(sample);
}

static const struct file_operations adxl345_custom_fops = {
	.owner = THIS_MODULE,
	.read = adxl345_custom_read,
	.llseek = noop_llseek,
};

static ssize_t adxl345_axis_show(struct device *kdev, char *buf, int axis)
{
	struct adxl345_dev *dev = i2c_get_clientdata(to_i2c_client(kdev));
	struct adxl345_custom_sample sample;
	int ret;
	s16 value;

	if (!dev)
		return -ENODEV;

	mutex_lock(&dev->lock);
	ret = adxl345_read_sample(dev, &sample);
	mutex_unlock(&dev->lock);
	if (ret)
		return ret;

	value = axis == 0 ? sample.x : axis == 1 ? sample.y : sample.z;
	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t x_raw_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	return adxl345_axis_show(kdev, buf, 0);
}
static DEVICE_ATTR_RO(x_raw);

static ssize_t y_raw_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	return adxl345_axis_show(kdev, buf, 1);
}
static DEVICE_ATTR_RO(y_raw);

static ssize_t z_raw_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	return adxl345_axis_show(kdev, buf, 2);
}
static DEVICE_ATTR_RO(z_raw);

static ssize_t lsb_per_g_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", ADXL345_CUSTOM_LSB_PER_G);
}
static DEVICE_ATTR_RO(lsb_per_g);

static ssize_t devid_show(struct device *kdev, struct device_attribute *attr, char *buf)
{
	struct adxl345_dev *dev = i2c_get_clientdata(to_i2c_client(kdev));
	u8 id = 0;
	int ret;

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

static struct attribute *adxl345_attrs[] = {
	&dev_attr_x_raw.attr,
	&dev_attr_y_raw.attr,
	&dev_attr_z_raw.attr,
	&dev_attr_lsb_per_g.attr,
	&dev_attr_devid.attr,
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

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->client = client;
	mutex_init(&dev->lock);
	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name = "adxl345_custom";
	dev->misc.fops = &adxl345_custom_fops;
	dev->misc.parent = &client->dev;
	dev->misc.mode = 0644;

	ret = adxl345_configure(client);
	if (ret)
		return ret;

	i2c_set_clientdata(client, dev);

	ret = misc_register(&dev->misc);
	if (ret)
		return dev_err_probe(&client->dev, ret, "misc_register failed\n");
	dev->misc_ready = true;

	dev_info(&client->dev, "bound as /dev/adxl345_custom\n");
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

	if (bus_num < 0 || addr > 0x7f)
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

MODULE_DESCRIPTION("Polled ADXL345 I2C driver (out of tree, id adxl345_custom)");
MODULE_LICENSE("GPL v2");
