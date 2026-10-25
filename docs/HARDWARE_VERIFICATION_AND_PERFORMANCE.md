# Hardware Verification and Performance Report — ADXL345

**Target:** Raspberry Pi Zero 2 W, Adafruit ADXL345 over **I2C-1** at address **0x53**  
**Evidence review:** 2026-10-25; retained Pi-on-hardware observations (**not rerun**)  
**Scope:** C++23 userspace driver, custom C loadable kernel module, GPIO17 data-ready IRQ / polled operation and comparison with Linux driver peers.

## 1. Configuration and acceptance criteria

Wiring uses **3.3 V**, SDA GPIO2, SCL GPIO3 and interrupt GPIO17 (Pi physical pin 11). The custom module uses full-resolution ±2 g, nominal **256 LSB/g**, output-data-rate **100 Hz**, a one-shot **six-byte I2C burst** at `DATAX0 (0x32)`, and a 24-byte sample ABI (axes, scale, sequence, overrun, monotonic timestamp).

Hardware checks require: DEVID `0xE5`; gravity magnitude **0.70–1.30 g**; driver-pair means within **0.15 g per axis** and **0.10 g magnitude**; near-100 Hz IRQ intervals; uninterrupted monotonic sequences; and no I2C errors during the captured test. These are **bounded acceptance checks**—not device calibration certification. [Test plan](testing.md), [kernel design](kernel.md), [wiring](wiring.md).

## 2. Recorded hardware results

| Test / run | Existing recorded result | Conclusion |
| --- | --- | --- |
| Unit and mock-bus regressions on Pi | **433 passed, 0 failed** | Software verification recorded in [README](../README.md) |
| C++ direct I2C hardware check | Device ID, `THRESH_TAP` readback, measure bit, 32-sample burst, standby passed; still-board mean magnitude **0.951 g**, sample-to-sample deviation **0.009 g** | Functional userspace path |
| Sensor self-test at 3.3 V | Axis shifts **+1.215 g**, **−1.262 g**, **+2.695 g**; recorded as clearing minimum limits | Sensor/self-test path exercised |
| Kernel peer comparison (32 samples each) | `adxl34x` mean (0.179, −0.098, 0.902) g / norm **0.925 g**; IIO mean (0.183, −0.069, 0.929) g / **0.950 g**; custom mean (0.183, −0.068, 0.929) g / **0.949 g** | IIO–custom axes differ ≈**0.001 g**; all within declared tolerances |
| IRQ series, 64 samples | Median inter-sample interval **9.95 ms**; every observed sequence increment **+1**; **0 I2C errors** | Observed ~100 Hz data-ready operation |
| Deliberately unread 300 ms interval | `irq_count` **123 → 153**; `dropped` **27 → 56**; chip overruns **0** | Overwrite accounting observed, **not** a zero-loss overload result |
| Polled kernel series, 32 samples | Mean (0.178, −0.065, 0.929) g / **0.948 g**; slowest read **0.92 ms**; `irq_count`, `dropped`, `i2c_errors` stayed at zero | Deliberate poll mode exercised |
| IRQ restored, 32 samples | Mean (0.179, −0.065, 0.930) g / **0.949 g**; median **9.95 ms**; max axis mean discrepancy poll vs IRQ ≈**0.001 g** | Poll/IRQ agreement within limits |

These results and their caveats are retained in [the README's “Checked on this Pi” section](../README.md#checked-on-this-pi), with procedures in [testing](testing.md) and [kernel](kernel.md). Sampling while the board sat still is not a calibrated dynamic-motion or environmental test.

## 3. CI scope and reproducibility

GitHub Actions installs **g++-14** and runs `make test CXX=g++-14`: mock-bus unit/regression suites plus compilation of Linux CLI and the hardware comparison/IRQ/mode test programs. The runner **does not load the custom kernel module or access real I2C/GPIO**. Kernel builds require matching Pi kernel headers.

```sh
make test
# On Pi with sensor connected and kernel headers available:
./build/adxl345_hw
./tools/run_kernel_compare.sh
# run_kernel_compare invokes comparison, IRQ, and poll-vs-IRQ tests
```

See [CI workflow](../.github/workflows/ci.yml) and [Pi deployment](pi.md). Only one kernel or userspace driver should own I2C address `0x53` at a time.

## 4. Disposition and gaps

**Supported:** measured still-board sensor/IRQ behavior, 32-sample cross-driver agreement, explicit 300 ms unread-data overrun accounting, and two acquisition modes on real hardware. **Not established:** timing distribution across temperature/power modes, worst-case kernel interrupt latency, long-term reliability, calibration traceability, moving/shock performance, extensive fault injection, or newly repeated test results on this report's date.
