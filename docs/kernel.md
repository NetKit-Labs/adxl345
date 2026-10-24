# Kernel module

`kernel/adxl345_custom.c` is a loadable module for the ADXL345 on `/dev/i2c-1`. It is not built into the kernel image. Load it with `insmod` and remove it with `rmmod`.

The userspace library under `src/` is still C++23. The module is a different program. Kernel modules on this tree are C, and Kbuild passes `-std=gnu11`. That is the newest dialect `scripts/Makefile.extrawarn` allows. C23 is not accepted. The module uses C11 `_Static_assert` and does not try to override `-std=`.

`MODULE_LICENSE` is `"GPL v2"`.

## What it does

The i2c id is `adxl345_custom`. It does not match `adxl34x` or `adxl345`, and it has no devicetree compatible string, so it does not take the in-tree drivers' devices.

On `insmod`, with the default parameters, it registers the driver and creates a client at `0x53` on adapter 1. Probe checks `DEVID` `0xE5`, writes zeros to `OFSX/Y/Z`, sets `BW_RATE` `0x0A` (100 Hz), `DATA_FORMAT` `0x08` (full resolution, ±2 g), disables interrupts, puts the FIFO in bypass, and sets `POWER_CTL` measure. Remove, and the devm cleanup, return the part to standby.

Full resolution keeps the scale at 256 LSB/g (3.9 mg/LSB) at every range. ±2 g is enough for gravity and matches the register value the IIO probe leaves behind (`DATA_FORMAT` `0x08`).

Reads use SMBus byte and I2C-block helpers. A sample is one 6-byte read at `DATAX0` (`0x32`). The SPI multi-byte bit is not set. The block helpers copy into a DMA-safe buffer because the Pi I2C adapter may DMA.

There is no IRQ path. INT1 and INT2 are not wired.

## Device node

`/dev/adxl345_custom` is mode `0644`. Each `read()` of at least 8 bytes returns one `struct adxl345_custom_sample` from `include/adxl345/custom_mod_abi.h`:

| Field | Type | Meaning |
| --- | --- | --- |
| `x`, `y`, `z` | `int16` | Counts, host endian |
| `lsb_per_g` | `uint16` | `256` |

A short read returns `-EINVAL`. The file offset is ignored, so a second `read()` on the same descriptor is the next sample.

Sysfs on `1-0053` (the i2c client) has `x_raw`, `y_raw`, `z_raw`, `lsb_per_g`, and `devid`. Each axis file does its own burst, so the three sysfs reads are not one sample. The char device is the coherent read.

```bash
make -C kernel
sudo insmod kernel/adxl345_custom.ko
# optional: bus_num=1 addr=0x53 autoprobe=1
sudo rmmod adxl345_custom
```

`autoprobe=0` only registers the driver. Bind later with:

```bash
echo adxl345_custom 0x53 | sudo tee /sys/bus/i2c/devices/i2c-1/new_device
```

## The other drivers in the comparison

| Driver | Where it comes from | Id | On this wiring |
| --- | --- | --- | --- |
| `adxl34x_i2c` | Shipped. `CONFIG_INPUT_ADXL34X_I2C=m` | `adxl34x` | Probe returns `-ENODEV` and logs `no IRQ?`. Samples are taken in the interrupt handler. |
| `adxl345_i2c` | Unmodified sources in `kernel/stock/` from raspberrypi/linux `rpi-6.18.y` `drivers/iio/accel/` | `adxl345` | Polls `in_accel_*_raw`. No IRQ required. This image did not ship the `.ko`. |
| `adxl345_custom` | `kernel/adxl345_custom.c` | `adxl345_custom` | Polls `/dev/adxl345_custom`. |

`kernel/stock/` is built with the same out-of-tree Kbuild invocation. Those files are not a second copy of this module. They are the existing IIO driver, compiled so the test can load it without rebuilding vmlinux.

`tools/run_kernel_compare.sh` builds all of that and runs `build/adxl345_compare`. The numeric check uses 256 LSB/g for both live drivers. The IIO sysfs scale is printed as well. With `DATA_FORMAT` `0x08` the IIO scale table's ±2 g entry is not 3.9 mg/LSB; the comparison still uses the datasheet full-resolution scale so the counts can be compared directly.

On this 6.18 kernel, writing `adxl345 0x53` to `new_device` creates the client but does not bind `adxl345_i2c` (`device_get_match_data()` comes back empty). The comparison then applies `kernel/adxl345-overlay.dts`, whose `adi,adxl345` node does bind. The custom module does not need that overlay; its init creates the client itself.

Only one driver can own `0x53`. The test instantiates them one after another. It leaves `adxl345_custom` loaded.
