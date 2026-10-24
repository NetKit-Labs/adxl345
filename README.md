# ADXL345 I2C driver

Two separate pieces for the Adafruit ADXL345 on a Raspberry Pi Zero 2 W:

- A C++23 userspace library. It opens `/dev/i2c-N` and uses `I2C_RDWR`.
- A loadable kernel module, `kernel/adxl345_custom.c`. It is C, built out of tree, and it is not the driver already in the kernel.

## Why

The kernel module is the device driver. `insmod` loads it. The kernel image is not rebuilt. The module uses the I2C core and the Pi's adapter driver already in that image, and it configures the ADXL345 by writing the chip's registers. The part has no processor and no firmware to load. After probe, userspace reads one sample at a time from `/dev/adxl345_custom`.

The C++23 library is a second program. It opens `/dev/i2c-1` itself. Its unit and regression tests run on a mock bus, so they do not need the board.

The image already ships an ADXL345 input driver, `adxl34x`, and the kernel tree also has an IIO driver, `adxl345_i2c`. The custom module uses its own id, `adxl345_custom`, so those drivers keep theirs. This image had not built the IIO module, so the comparison compiles that unmodified source and reads it beside `adxl345_custom` while the board sits still. Details are in [docs/kernel.md](docs/kernel.md). SPI is not implemented.

## What you get

| Piece | Role |
| --- | --- |
| `lib` via `src/driver.cpp` | Register access, samples in g, FIFO, tap, activity, free-fall, offsets, self-test |
| `build/adxl345` | CLI: scan, identify, read, dump, self-test, calibrate |
| `build/adxl345_tests` | Unit tests and datasheet regression tests on a mock bus. No hardware. |
| `build/adxl345_hw` | On-target checks: device id, register readback, gravity, self-test |
| `kernel/adxl345_custom.c` | Loadable kernel module. Device node `/dev/adxl345_custom` |
| `build/adxl345_compare` | Linux-only C++23 program. Loads each kernel driver and compares samples |

Docs: [wiring](docs/wiring.md), [registers](docs/registers.md), [API](docs/api.md), [tests](docs/testing.md), [kernel module](docs/kernel.md), [Pi](docs/pi.md).

## Wiring

Four wires. INT1 and INT2 are left open.

| Breakout | Pi Zero 2 W |
| --- | --- |
| VIN | Pin 1, 3.3 V |
| GND | Pin 6 |
| SDA | Pin 3, GPIO2 |
| SCL | Pin 5, GPIO3 |

CS and SDO stay unconnected. The Adafruit board pulls CS high, which selects I2C, and pulls SDO low, which selects address `0x53`. Pin-level notes are in [docs/wiring.md](docs/wiring.md).

## Polling

The C++ library and `adxl345_custom` both poll. A sample is one 6-byte I2C read at `DATAX0` (`0x32`). The module sets the output rate to 100 Hz, disables the chip's interrupt outputs, and does not request a GPIO line. That matches this wiring: with INT1 and INT2 open, nothing can wake the Pi.

The input driver shipped on the image, `adxl34x`, binds only when an interrupt is present. On this board it logs `no IRQ?` and takes no samples. The IIO driver polls when no interrupt is described, which is the path the comparison used.

Polling is a normal way to take a slow reading. A continuous stream, or tap, activity, and free-fall, would use INT1 or INT2.

## Build

Host or Pi, with g++ 14 (C++23) or a similarly current Clang:

```bash
make test
```

On Linux that also builds `build/adxl345`, `build/adxl345_hw`, and `build/adxl345_compare`. CMake is the same layout:

```bash
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
```

`make test` runs both suites. Unit tests cover scale factors, rounding, and error paths. Regression tests lock register addresses, bit fields, the datasheet's offset example, and the exact I2C transactions the driver issues (one 6-byte read at `0x32`, not six single-byte reads, and not the SPI multiple-byte bit).

GitHub Actions runs that same `make test` on Ubuntu with g++ 14. The job also compiles the Linux CLI, the hardware program, and the kernel comparison program. It does not load `adxl345_custom`, and it does not open an I2C bus. Those steps need the Pi and the breakout.

## Run it on the Pi

The Pi Zero 2 W used with the netkit board tests is `pi.local` (user `pi`). I2C has to be enabled once:

```bash
sudo raspi-config nonint do_i2c 0
```

From the Mac, with the same `NETKIT_PI_PASS` the netkit scripts use:

```bash
export NETKIT_PI_HOST=pi.local
export NETKIT_PI_USER=pi
export NETKIT_PI_PASS='…'
./tools/deploy.sh
```

That enables I2C, copies the tree to `/home/pi/adxl345`, builds with the Pi's g++, runs the unit and regression suites, scans the bus, and runs the hardware checks.

On the Pi itself:

```bash
make all test
./build/adxl345 scan
./build/adxl345 read --count 20
./build/adxl345 self-test --vs 3.3
./build/adxl345_hw
```

`ADXL345_BUS`, `ADXL345_ADDR`, and `ADXL345_VS` override the hardware test. Default bus is `/dev/i2c-1`, and the probe tries `0x53` (Adafruit default, SDO pulled down) then `0x1D`.

## Library sketch

```cpp
#include "adxl345/adxl345.hpp"

adxl345::LinuxI2cBus bus("/dev/i2c-1", adxl345::reg::address_sdo_low);
adxl345::Adxl345 sensor(bus);
sensor.verify();  // DEVID must be 0xE5
sensor.set_data_format({.full_resolution = true, .range = adxl345::Range::g16});
sensor.set_data_rate(adxl345::DataRate::hz100);
sensor.start();
adxl345::Acceleration g = sensor.read_g_when_ready(std::chrono::milliseconds(50));
```

The part wakes in standby. `start()` sets `POWER_CTL.measure`. `read_raw()` is a repeated-start burst of `DATAX0` through `DATAZ1` so the three axes belong to one sample. Counts are nominal 256 LSB/g in full resolution (3.9 mg/LSB). See [docs/api.md](docs/api.md).

## Kernel module

Kbuild compiles `kernel/adxl345_custom.c` with `-std=gnu11`. The kernel build sets that flag (`scripts/Makefile.extrawarn`). It is the newest C dialect this kernel accepts for a module, so the Makefile does not pass another `-std=`. The file uses C11 `_Static_assert`. It is not C++.

```bash
make -C kernel
sudo insmod kernel/adxl345_custom.ko
```

`insmod` binds i2c-1 address `0x53` and creates `/dev/adxl345_custom`. Parameters are `bus_num` (default 1), `addr` (default `0x53`), and `autoprobe` (default true). The id is `adxl345_custom`, so this module does not claim `adxl34x` or `adxl345`.

A `read()` of 8 bytes returns one sample: three native-endian `int16` counts and a `uint16` scale of 256 LSB/g. The i2c device's sysfs exposes `x_raw`, `y_raw`, `z_raw`, `lsb_per_g`, and `devid`. Each `read()` is a new sample. The file offset is ignored.

On the Pi:

```bash
./tools/run_kernel_compare.sh
```

That needs passwordless sudo. It builds this module and the unmodified IIO driver, loads the shipped `adxl34x` driver, then the IIO driver, then this module, and prints both sets of samples. It leaves `adxl345_custom` loaded when that module probes. `sudo rmmod adxl345_custom` before using the C++23 library on `/dev/i2c-1`. Only one owner of address `0x53` at a time.

`./tools/deploy.sh` from the Mac runs the userspace tests and then this comparison.

## Checked on this Pi

I2C is enabled (`sudo raspi-config nonint do_i2c 0`). The breakout answers at `0x53` on `/dev/i2c-1`.

On the Pi, `make test` reported `392 passed, 0 failed`. `build/adxl345_hw` then passed device id, `THRESH_TAP` readback, the measure bit, a 32-sample burst, and standby. The board was sitting still: mean magnitude **0.980 g**, sample-to-sample deviation **0.011 g**. Self-test at 3.3 V shifted the axes by **+1.207 g, −1.266 g, +2.676 g**, which clears the datasheet minimum.

`build/adxl345_compare` then loaded the kernel drivers. Kbuild recorded `-std=gnu11` for `adxl345_custom.ko`. The shipped `adxl34x` driver logged `no IRQ?` and did not bind, so it produced no samples. The IIO driver and `adxl345_custom` both did. Over 32 samples their means were **0.680, 0.270, 0.651 g** (IIO, magnitude 0.979 g) and **0.679, 0.269, 0.651 g** (custom, magnitude 0.978 g). The difference was about **0.001 g** per axis. `adxl345_custom` was left loaded on `/dev/adxl345_custom`.

## Future work

Wire INT1 or INT2 to a free GPIO and handle that line in `adxl345_custom`. Data-ready, or a FIFO watermark, is the usual path for a continuous stream. Tap, activity, inactivity, and free-fall use the same pin: the chip watches the thresholds and wakes the host when one fires. Registering the module with IIO would match the kernel's usual accelerometer interface. The C++ library already has helpers for those registers. The kernel module does not use them yet.

## Assumptions

- Four wires: 3.3 V, ground, SDA (GPIO2), SCL (GPIO3). CS stays pulled high on the Adafruit board. Details in [docs/wiring.md](docs/wiring.md).
- Pi I2C clock is the usual 100 kHz, so output data rates above 200 Hz can drop samples. 1600 Hz and 3200 Hz are rejected unless you pass `HighRatePolicy::allow`.
- Self-test limits are the datasheet tables at 2.5 V, scaled by the supply table for 2.0 V to 3.6 V. The hardware test defaults to 3.3 V. If the breakout regulator is fed from the Pi's 3.3 V pin, the sensor rail can be closer to 3.0 V; set `ADXL345_VS` or `--vs` if the shift is low.
- One `Adxl345` object is not internally locked.
- The kernel module and `build/adxl345` talk to the same chip. Run one at a time. `rmmod adxl345_custom` before the userspace I2C program, and do not `insmod` while that program has `/dev/i2c-1` open.
