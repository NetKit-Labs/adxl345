#pragma once

#include "adxl345/codec.hpp"
#include "adxl345/i2c_bus.hpp"

#include <chrono>
#include <cstdint>

namespace adxl345 {

// Userspace driver for an ADXL345 sitting on a Linux I2C bus.
// One instance is not synchronized; call it from one thread.
//
// The part powers up in standby. Acceleration registers stay meaningless until
// `start()` sets the measure bit. A burst read of DATAX0..DATAZ1 is the only
// read that keeps a single sample's three axes together.
class Adxl345 {
public:
    explicit Adxl345(I2cBus& bus);

    void verify();
    [[nodiscard]] std::uint8_t device_id();

    void set_data_format(DataFormat format);
    [[nodiscard]] DataFormat data_format();

    void set_data_rate(DataRate rate, bool low_power = false,
                       HighRatePolicy policy = HighRatePolicy::reject_above_800hz);
    [[nodiscard]] DataRate data_rate();
    [[nodiscard]] bool low_power();

    void set_power(PowerControl power);
    [[nodiscard]] PowerControl power();
    void start();
    void standby();
    [[nodiscard]] bool measuring();

    void set_offsets(AxisOffsets offsets);
    [[nodiscard]] AxisOffsets offsets();

    // Whole-register writes. Bits set in `int2_mask` are routed to INT2; the rest go to INT1.
    void set_interrupts_enabled(std::uint8_t mask);
    void map_interrupts(std::uint8_t int2_mask);
    [[nodiscard]] std::uint8_t interrupt_source();
    [[nodiscard]] ActTapStatus activity_tap_status();

    void configure_tap(const TapSettings& settings);
    void configure_activity(const ActivitySettings& settings);
    void configure_free_fall(const FreeFallSettings& settings);

    void set_fifo(FifoControl fifo);
    [[nodiscard]] FifoStatus fifo_status();

    void set_self_test(bool enabled);

    [[nodiscard]] RawSample read_raw();
    [[nodiscard]] Acceleration read_g();

    // Polls INT_SOURCE for DATA_READY. That read also clears the latched
    // tap, activity, inactivity, and free-fall bits.
    [[nodiscard]] RawSample read_raw_when_ready(std::chrono::milliseconds timeout);
    [[nodiscard]] Acceleration read_g_when_ready(std::chrono::milliseconds timeout);

    // Zeros the offset registers, reads `samples` right-justified counts, and
    // programs the offset that cancels the error versus `ideal_counts`.
    [[nodiscard]] AxisOffsets calibrate(int samples, RawSample ideal_counts);

    // Forces full-resolution ±16 g, 100 Hz, normal power, then restores the
    // previous format, rate, and power control — including when a read throws.
    [[nodiscard]] SelfTestResult run_self_test(SelfTestOptions options = {});

    [[nodiscard]] std::uint8_t read_register(std::uint8_t reg);
    void write_register(std::uint8_t reg, std::uint8_t value);

private:
    void write8(std::uint8_t reg, std::uint8_t value);
    [[nodiscard]] std::uint8_t read8(std::uint8_t reg);
    [[nodiscard]] RawSample average_samples(int samples);

    I2cBus& bus_;
};

}  // namespace adxl345
