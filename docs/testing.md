# Tests

Three layers. `make test` always builds and runs the mock-bus suites. On Linux it also compiles the CLI, `build/adxl345_hw`, `build/adxl345_compare`, `build/adxl345_irq`, and `build/adxl345_modes`, and it does not run those programs.

## Unit tests

`adxl345_tests unit` calls the pure helpers and a few driver paths on `MockBus`.

- LSB/g and resolution for every range, with and without full resolution
- The 16 output data rates and the matching bandwidths
- DATA_FORMAT and POWER_CTL encode/decode across the bit combinations
- Little-endian decode, including negative Y and Z
- Left-justified normalization for 10-bit and 13-bit words
- g conversion at 256, 128, and 32 LSB/g
- The datasheet offset example: +10, −13, +9 LSB becomes −3, +3, −2, and the ratio changes in 10-bit ±16 g
- Quantizers for 62.5 mg, 625 µs, 1.25 ms, 5 ms, and 1 s
- Self-test Tables 15–18 at 2.5 V, the 3.3 V scale (X minimum 89 LSB, Z maximum 1286 LSB), and rejection of 1.9 V and 3.7 V
- A wrong DEVID throws `Error::bad_device_id` after a single read of register `0x00`
- Standby samples read as zero; measurement samples convert
- An injected bus error surfaces as `std::system_error`
- The kernel sample is 24 bytes. `sequence` is at offset 8, `overruns` at 12, and `timestamp_ns` at 16. INT1 maps to `0x00`, INT2 maps data-ready and overrun to `0x81`, and the level-high flag is 4
- The first published interrupt sample does not count as dropped. A second publish before that sample is taken does

## Regression tests

`adxl345_tests regression` fails if a bit moves or a transaction changes shape.

- Every used address, from `DEVID` `0x00` through `FIFO_STATUS` `0x39`, plus `0xE5`, `0x53`, and `0x1D`
- `read_raw()` is one 6-byte read at `0x32` and not `0x72` (that would be the SPI multiple-byte bit)
- `start()` sets measure without clearing a link bit that was already set (`0x28`)
- Leaving sleep writes standby first (`0x01`) and then measure (`0x09`) when the wakeup code is 4 Hz
- Full-resolution ±16 g is `DATA_FORMAT` `0x0B`. 100 Hz low-power is `BW_RATE` `0x1A`. 3200 Hz throws, and `HighRatePolicy::allow` writes `0x0F`
- Left-justified ±2 g 10-bit places a count of 1 at `0x0040` and −1 at `0xFFC0`, and `read_g()` still returns those counts in g
- Calibration of the datasheet example writes `OFSX/Y/Z` as `FD 03 FE` in one 3-byte transfer
- Default tap, activity, and free-fall settings land on the scale factors above. FIFO samples of 32 are rejected with no write. Stream mode, INT2 trigger, watermark 16 is `FIFO_CTL` `0xB0`
- `INT_ENABLE` data-ready plus overrun is `0x81`. Mapping single-tap to INT2 is `0x40`. Reading `INT_SOURCE` clears bits `0x7C` and leaves data-ready, watermark, and overrun. The following data burst clears data-ready.
- `read_raw_when_ready(0ms)` times out until a sample sets data-ready, then returns that sample
- Self-test on the mock applies a +100, −100, +200 LSB shift, reports a pass against Table 15, writes `0x0B` then `0x8B`, and restores format `0x00`, rate `0x0A`, and power `0x00`
- A 10 LSB shift fails the minimum. A 5 V supply argument throws before any bus transaction
- Four publishes with no reader leave `dropped` at 3. Catching up and publishing one sample per read adds no drops. Two further publishes with no read add one drop, and `sequence` is 11

`make test` and `ctest` run both suites. The pass line is `N passed, 0 failed`.

## Hardware test

`build/adxl345_hw` talks to the breakout.

1. Probe `0x53` then `0x1D` for `DEVID` `0xE5`.
2. Write `0x55` to `THRESH_TAP`, read it back, restore the previous value.
3. Zero offsets for the duration of the test, select full-resolution ±16 g at 100 Hz, and set measure.
4. Take 32 samples synced to `DATA_READY`. The mean magnitude must sit between 0.45 g and 1.75 g, and no sample may be more than 1 g away from that mean.
5. Run self-test at `ADXL345_VS` (default 3.3). The check passes on the datasheet minimum shift. A shift above the maximum is printed and does not fail the run.
6. Return to standby and restore the offset registers.

Exit 0 when every check passed, 1 when a check failed, 2 when the bus node or the part is missing.

```bash
ADXL345_VS=3.0 ./build/adxl345_hw
```

Use 3.0 V if VIN is the Pi's 3.3 V pin and the breakout regulator drops a few hundred millivolts. The gravity check does not depend on that guess.

## Kernel drivers

`build/adxl345_compare` is Linux-only. `make test` compiles it there and does not run it. `tools/run_kernel_compare.sh` builds the modules and runs the comparison as root.

The program frees address `0x53`, then:

1. `modprobe adxl34x-i2c` and apply `kernel/adxl34x-overlay.dts`, which describes GPIO17 as a level-high interrupt. This is the driver the Pi image ships. Its IRQ handler fills the `position` sysfs file. The input device is held open so the chip stays in measure mode.
2. `insmod` the unmodified IIO modules from `kernel/stock/`, instantiate `adxl345 0x53` (or a small overlay if sysfs does not supply match data), zero `in_accel_*_calibbias`, and read 32 samples of `in_accel_*_raw`.
3. `insmod kernel/adxl345_custom.ko` and take 32 samples from `/dev/adxl345_custom`.

The three series are converted at 256 LSB/g. The run passes when each magnitude sits between 0.70 g and 1.30 g, each axis of the IIO driver and of `adxl34x` differs from `adxl345_custom` by at most 0.15 g, and the magnitudes differ by at most 0.10 g. Exit 0 on that pass, 1 when the drivers sampled but disagree, 2 when a driver produced no samples.

The custom module stays loaded after a probe so `/dev/adxl345_custom` can be read again. Remove it before `build/adxl345` or `build/adxl345_hw`, which open `/dev/i2c-1` themselves.

`build/adxl345_irq` runs after that, against the module left loaded. It requires `mode` `irq` on GPIO17. It takes 64 blocking reads and checks that the handler's timestamps sit near 10 ms, that sequence numbers advance by one, and that the mean magnitude is about 1 g. It then pauses 300 ms and checks that `irq_count` and `dropped` keep moving while userspace is not reading. `i2c_errors` must stay at zero.

`build/adxl345_modes` then reloads that module with `irq_gpio=-1`. It requires sysfs `mode` `poll`, `irq_gpio` `-1`, `int_line` `0`, and `trigger` `none`. `poll()` on `/dev/adxl345_custom` must report readable at once. Thirty-two reads, spaced about 10 ms apart, must each return in well under 50 ms, advance the sequence by 1, and leave `irq_count`, `dropped`, and `i2c_errors` at 0. The mean magnitude must sit between 0.70 g and 1.30 g. The program then loads the module again with the default GPIO17 interrupt, takes 32 blocking samples, and requires the same gravity band and a median interval near 10 ms. The two means must agree within 0.15 g on each axis and 0.10 g in magnitude. It leaves `adxl345_custom` loaded in interrupt mode. Exit 0 on that pass, 1 when a check fails, 2 when the module cannot be loaded, 3 when the program is not run as root.

## GitHub Actions

`.github/workflows/ci.yml` runs on each push and pull request. The runner is Ubuntu with g++ 14. `make test` builds the C++23 library, runs the unit and regression suites on the mock bus, and compiles `build/adxl345`, `build/adxl345_hw`, `build/adxl345_compare`, `build/adxl345_irq`, and `build/adxl345_modes`.

The kernel module is not built there. It needs the Pi's kernel headers, and loading it needs the ADXL345 on i2c-1. `build/adxl345_hw`, `build/adxl345_compare`, `build/adxl345_irq`, and `build/adxl345_modes` are compiled so a break in those files fails CI, and they are not executed.
