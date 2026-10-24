#pragma once

#include "adxl345/registers.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace adxl345 {

enum class Range : std::uint8_t { g2 = 0, g4 = 1, g8 = 2, g16 = 3 };

// Rate-code field of BW_RATE. The enumerator value is the register field.
enum class DataRate : std::uint8_t {
    hz0_10 = 0x0,
    hz0_20 = 0x1,
    hz0_39 = 0x2,
    hz0_78 = 0x3,
    hz1_56 = 0x4,
    hz3_13 = 0x5,
    hz6_25 = 0x6,
    hz12_5 = 0x7,
    hz25 = 0x8,
    hz50 = 0x9,
    hz100 = 0xA,
    hz200 = 0xB,
    hz400 = 0xC,
    hz800 = 0xD,
    hz1600 = 0xE,
    hz3200 = 0xF,
};

enum class WakeupFrequency : std::uint8_t { hz8 = 0, hz4 = 1, hz2 = 2, hz1 = 3 };

enum class FifoMode : std::uint8_t { bypass = 0, fifo = 1, stream = 2, trigger = 3 };

// 1600 Hz and 3200 Hz are outside the I2C output-data-rate limit in the datasheet.
enum class HighRatePolicy { reject_above_800hz, allow };

struct RawSample {
    std::int16_t x{};
    std::int16_t y{};
    std::int16_t z{};
    bool operator==(const RawSample&) const = default;
};

struct Acceleration {
    double x_g{};
    double y_g{};
    double z_g{};

    [[nodiscard]] double magnitude_g() const;
};

struct DataFormat {
    bool self_test = false;
    bool spi_3wire = false;
    bool interrupt_active_low = false;
    bool full_resolution = true;
    bool left_justified = false;
    Range range = Range::g2;
    bool operator==(const DataFormat&) const = default;
};

struct PowerControl {
    bool link = false;
    bool auto_sleep = false;
    bool measure = false;
    bool sleep = false;
    WakeupFrequency wakeup = WakeupFrequency::hz8;
    bool operator==(const PowerControl&) const = default;
};

struct FifoControl {
    FifoMode mode = FifoMode::bypass;
    bool trigger_int2 = false;
    std::uint8_t samples = 0;
    bool operator==(const FifoControl&) const = default;
};

struct FifoStatus {
    bool triggered = false;
    std::uint8_t entries = 0;
    bool operator==(const FifoStatus&) const = default;
};

struct AxisOffsets {
    std::int8_t x = 0;
    std::int8_t y = 0;
    std::int8_t z = 0;
    bool operator==(const AxisOffsets&) const = default;
};

struct ActTapStatus {
    bool activity_x = false;
    bool activity_y = false;
    bool activity_z = false;
    bool asleep = false;
    bool tap_x = false;
    bool tap_y = false;
    bool tap_z = false;
    bool operator==(const ActTapStatus&) const = default;
};

struct TapSettings {
    double threshold_g = 3.0;
    double duration_s = 0.010;
    double latency_s = 0.020;
    double window_s = 0.080;
    bool suppress = false;
    bool x = true;
    bool y = true;
    bool z = true;
};

struct ActivitySettings {
    double activity_threshold_g = 0.5;
    double inactivity_threshold_g = 0.5;
    double inactivity_time_s = 1.0;
    bool activity_ac = false;
    bool inactivity_ac = false;
    bool activity_x = true;
    bool activity_y = true;
    bool activity_z = true;
    bool inactivity_x = true;
    bool inactivity_y = true;
    bool inactivity_z = true;
};

struct FreeFallSettings {
    // Datasheet suggests 300 mg to 600 mg and 100 ms to 350 ms.
    double threshold_g = 0.4375;
    double time_s = 0.160;
};

struct SelfTestLimits {
    int min_x = 0;
    int max_x = 0;
    int min_y = 0;
    int max_y = 0;
    int min_z = 0;
    int max_z = 0;
    bool operator==(const SelfTestLimits&) const = default;
};

struct SelfTestResult {
    RawSample off{};
    RawSample on{};
    std::int32_t x_lsb = 0;
    std::int32_t y_lsb = 0;
    std::int32_t z_lsb = 0;
    double x_g = 0;
    double y_g = 0;
    double z_g = 0;
    SelfTestLimits limits{};
    bool within_window = false;
    bool meets_minimum = false;
};

struct SelfTestOptions {
    int samples = 10;
    int discard = 4;
    // Four output samples at 100 Hz is 40 ms. 50 ms covers that settling note.
    int settle_ms = 50;
    double supply_volts = 3.3;
};

class Error : public std::runtime_error {
public:
    enum class Code { bad_device_id, timeout };

    Error(Code code, const std::string& message) : std::runtime_error(message), code_(code) {}

    [[nodiscard]] Code code() const noexcept { return code_; }

private:
    Code code_;
};

[[nodiscard]] constexpr std::uint8_t encode(DataFormat format) noexcept {
    std::uint8_t value = static_cast<std::uint8_t>(format.range) & reg::fmt_range_mask;
    if (format.left_justified) {
        value = static_cast<std::uint8_t>(value | reg::fmt_justify);
    }
    if (format.full_resolution) {
        value = static_cast<std::uint8_t>(value | reg::fmt_full_res);
    }
    if (format.interrupt_active_low) {
        value = static_cast<std::uint8_t>(value | reg::fmt_int_invert);
    }
    if (format.spi_3wire) {
        value = static_cast<std::uint8_t>(value | reg::fmt_spi_3wire);
    }
    if (format.self_test) {
        value = static_cast<std::uint8_t>(value | reg::fmt_self_test);
    }
    return value;
}

[[nodiscard]] constexpr DataFormat decode_data_format(std::uint8_t value) noexcept {
    DataFormat format;
    format.self_test = (value & reg::fmt_self_test) != 0;
    format.spi_3wire = (value & reg::fmt_spi_3wire) != 0;
    format.interrupt_active_low = (value & reg::fmt_int_invert) != 0;
    format.full_resolution = (value & reg::fmt_full_res) != 0;
    format.left_justified = (value & reg::fmt_justify) != 0;
    format.range = static_cast<Range>(value & reg::fmt_range_mask);
    return format;
}

[[nodiscard]] constexpr std::uint8_t encode(PowerControl power) noexcept {
    std::uint8_t value = static_cast<std::uint8_t>(power.wakeup) & reg::power_wakeup_mask;
    if (power.sleep) {
        value = static_cast<std::uint8_t>(value | reg::power_sleep);
    }
    if (power.measure) {
        value = static_cast<std::uint8_t>(value | reg::power_measure);
    }
    if (power.auto_sleep) {
        value = static_cast<std::uint8_t>(value | reg::power_auto_sleep);
    }
    if (power.link) {
        value = static_cast<std::uint8_t>(value | reg::power_link);
    }
    return value;
}

[[nodiscard]] constexpr PowerControl decode_power(std::uint8_t value) noexcept {
    PowerControl power;
    power.link = (value & reg::power_link) != 0;
    power.auto_sleep = (value & reg::power_auto_sleep) != 0;
    power.measure = (value & reg::power_measure) != 0;
    power.sleep = (value & reg::power_sleep) != 0;
    power.wakeup = static_cast<WakeupFrequency>(value & reg::power_wakeup_mask);
    return power;
}

[[nodiscard]] constexpr std::uint8_t encode_bw_rate(DataRate rate, bool low_power) noexcept {
    std::uint8_t value = static_cast<std::uint8_t>(rate) & reg::bw_rate_mask;
    if (low_power) {
        value = static_cast<std::uint8_t>(value | reg::bw_low_power);
    }
    return value;
}

[[nodiscard]] constexpr std::uint8_t encode(FifoControl fifo) noexcept {
    auto value = static_cast<std::uint8_t>(
        (static_cast<std::uint8_t>(fifo.mode) << reg::fifo_mode_shift) |
        (fifo.samples & reg::fifo_samples_mask));
    if (fifo.trigger_int2) {
        value = static_cast<std::uint8_t>(value | reg::fifo_trigger);
    }
    return value;
}

[[nodiscard]] constexpr FifoControl decode_fifo(std::uint8_t value) noexcept {
    FifoControl fifo;
    fifo.mode = static_cast<FifoMode>((value >> reg::fifo_mode_shift) & 0x03u);
    fifo.trigger_int2 = (value & reg::fifo_trigger) != 0;
    fifo.samples = static_cast<std::uint8_t>(value & reg::fifo_samples_mask);
    return fifo;
}

[[nodiscard]] constexpr FifoStatus decode_fifo_status(std::uint8_t value) noexcept {
    FifoStatus status;
    status.triggered = (value & reg::fifo_trig_status) != 0;
    status.entries = static_cast<std::uint8_t>(value & reg::fifo_entries_mask);
    return status;
}

[[nodiscard]] constexpr ActTapStatus decode_act_tap_status(std::uint8_t value) noexcept {
    ActTapStatus status;
    status.activity_x = (value & reg::status_act_x) != 0;
    status.activity_y = (value & reg::status_act_y) != 0;
    status.activity_z = (value & reg::status_act_z) != 0;
    status.asleep = (value & reg::status_asleep) != 0;
    status.tap_x = (value & reg::status_tap_x) != 0;
    status.tap_y = (value & reg::status_tap_y) != 0;
    status.tap_z = (value & reg::status_tap_z) != 0;
    return status;
}

}  // namespace adxl345
