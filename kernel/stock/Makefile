# Unmodified drivers/iio/accel/adxl345.h, adxl345_core.c, and adxl345_i2c.c
# from raspberrypi/linux branch rpi-6.18.y. This Pi image left CONFIG_ADXL345
# unset, so the comparison builds that existing driver here instead of
# rebuilding the kernel. The sources are not a second implementation.

obj-m += adxl345_core.o
obj-m += adxl345_i2c.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
