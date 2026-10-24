# API

Everything public lives in namespace `adxl345`. The umbrella header is `adxl345/adxl345.hpp`. On Linux it also pulls in `LinuxI2cBus`.

## Bus

`I2cBus` is the seam the tests replace.

```cpp
class I2cBus {
public:
    virtual void write(std::uint8_t reg, std::span<const std::uint8_t> data) = 0;
    virtual void read(std::uint8_t reg, std::span<std::uint8_t> data) = 0;
};
```

`LinuxI2cBus` opens a device node and one 7-bit address. Transfers use `I2C_RDWR`. Failures throw `std::system_error`. `probe_adxl345(path)` returns `0x53` or `0x1D` when `DEVID` is `0xE5`.

## Driver

```cpp
explicit Adxl345(I2cBus& bus);
void verify();                         // throws Error::bad_device_id
void set_data_format(DataFormat);
void set_data_rate(DataRate, bool low_power = false,
                   HighRatePolicy policy = HighRatePolicy::reject_above_800hz);
void set_power(PowerControl);          // standby-first when leaving sleep, link, or auto-sleep
void start();                          // set measure, keep the other power bits
void standby();
void set_offsets(AxisOffsets);
void set_interrupts_enabled(std::uint8_t mask);
void map_interrupts(std::uint8_t int2_mask);
std::uint8_t interrupt_source();       // also clears latched tap/activity/free-fall bits
void configure_tap(const TapSettings&);
void configure_activity(const ActivitySettings&);
void configure_free_fall(const FreeFallSettings&);
void set_fifo(FifoControl);            // samples must be 0..31
RawSample read_raw();                  // six bytes at 0x32
Acceleration read_g();
RawSample read_raw_when_ready(std::chrono::milliseconds timeout);
AxisOffsets calibrate(int samples, RawSample ideal_counts);
SelfTestResult run_self_test(SelfTestOptions options = {});
```

`Error` is used for a wrong device id and for `DATA_READY` timeout. Out-of-range settings throw `std::invalid_argument`.

`read_g()` reads `DATA_FORMAT`, then the burst, then converts. Right-justified data is already a sign-extended int16. Left-justified data is shifted down by `16 - resolution_bits`.

`calibrate()` writes zeros to the offset registers, averages `samples` normalized counts, and writes the offset that cancels the error against `ideal_counts`. For a still board in full resolution with Z up, the ideal sample is `{0, 0, 256}`. The worked example in the datasheet (errors of +10, −13, +9 LSB) becomes offsets −3, +3, −2, stored as `0xFD 0x03 0xFE`.

`run_self_test()` forces full-resolution ±16 g, 100 Hz, normal power, and measure. It averages samples with self-test off and on, restores the previous format, rate, and power even if a later read throws, and compares the difference with the datasheet limits at `options.supply_volts`. `meets_minimum` is the datasheet's "generally a pass" check. `within_window` also enforces the maximum. A shift past the maximum is not, by itself, called a failure in the datasheet.

## CLI

Built as `build/adxl345` on Linux.

```bash
adxl345 scan [--bus /dev/i2c-1]
adxl345 id [--addr 0x53]
adxl345 read --count 20 --rate 100 --range 16
adxl345 dump
adxl345 self-test --vs 3.3
adxl345 calibrate            # board still, +1 g on Z
adxl345 calibrate --z-down
```

`--10-bit` selects fixed 10-bit mode. `--low-power` sets BW_RATE D4. Rates above 800 Hz are not offered on the command line.

## Pure helpers

These do not touch a bus, and the unit tests call them directly.

| Function | Role |
| --- | --- |
| `nominal_lsb_per_g` | 256 / 128 / 64 / 32, or 256 in full resolution |
| `data_rate_hz`, `bandwidth_hz` | Table 7 |
| `max_recommended_odr_hz` | `i2c_clock_hz * 800 / 400000` |
| `encode` / `decode_*` | DATA_FORMAT, POWER_CTL, BW_RATE, FIFO |
| `decode_le`, `normalize`, `counts_to_g` | Sample path |
| `quantize_*` | 62.5 mg, 625 µs, 1.25 ms, 5 ms, 1 s steps |
| `compute_offsets` | 15.6 mg offset LSB, half away from zero |
| `self_test_limits` | Tables 14–18 |

Nominal conversion uses 256 LSB/g. At 3.3 V the datasheet's typical X/Y sensitivity moves to about 265 LSB/g and Z stays 256. The driver does not apply that second-order correction; pass it yourself if you need it.

## Kernel sample

`/dev/adxl345_custom` returns one `struct adxl345_custom_sample` from `include/adxl345/custom_mod_abi.h` (24 bytes). The same header holds the rules the module uses for the interrupt map and for counting an unread sample:

| Function | Role |
| --- | --- |
| `adxl345_custom_int_map` | `0` for INT1, `0x81` for INT2 (data-ready and overrun) |
| `adxl345_custom_note_publish` | Next sequence number. Counts a drop when the previous sample was not taken |

The module, the comparison program, and the mock-bus tests all include that header. With GPIO17 armed, `read()` blocks until the handler publishes a sample. With `irq_gpio=-1`, `read()` fetches the axes during the call and `poll()` reports the device readable immediately. `build/adxl345_modes` checks both on the Pi. Register programming for the C++ library is above. The load path, GPIO17, and the image-driver overlay are in [kernel.md](kernel.md).
