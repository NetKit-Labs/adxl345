# Out-of-tree module. Kbuild forces -std=gnu11 (see scripts/Makefile.extrawarn
# in the kernel tree). That is the newest C dialect this kernel allows a
# module to be built with; do not append another -std= flag.

obj-m += adxl345_custom.o

KDIR ?= /lib/modules/$(shell uname -r)/build

ccflags-y += -I$(src)/../include

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
