#pragma once

#include "adxl345/types.hpp"

#include <cstdint>
#include <span>

namespace adxl345 {

// Nominal scale from datasheet Table 1. 3.9 mg/LSB is 1/256 g.
// Full-resolution mode keeps that scale at every range. Fixed 10-bit mode
// halves the counts per g each time the range doubles.
[[nodiscard]] constexpr int nominal_lsb_per_g(Range range, bool full_resolution) noexcept {
    if (full_resolution || range == Range::g2) {
        return 256;
    }
    switch (range) {
        case Range::g4:
            return 128;
        case Range::g8:
            return 64;
        case Range::g16:
            return 32;
        case Range::g2:
            return 256;
    }
    return 256;
}

[[nodiscard]] constexpr int resolution_bits(Range range, bool full_resolution) noexcept {
    if (!full_resolution) {
        return 10;
    }
    switch (range) {
        case Range::g2:
            return 10;
        case Range::g4:
            return 11;
        case Range::g8:
            return 12;
        case Range::g16:
            return 13;
    }
    return 10;
}

[[nodiscard]] constexpr double data_rate_hz(DataRate rate) noexcept {
    constexpr double kHz[] = {0.10, 0.20, 0.39, 0.78, 1.56, 3.13, 6.25, 12.5,
                              25.0, 50.0, 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0};
    return kHz[static_cast<std::uint8_t>(rate)];
}

// Table 7 bandwidth column. It is ODR/2, with the low-rate entries rounded
// the way the datasheet prints them.
[[nodiscard]] constexpr double bandwidth_hz(DataRate rate) noexcept {
    constexpr double kHz[] = {0.05, 0.10, 0.20, 0.39, 0.78, 1.56, 3.13, 6.25,
                              12.5, 25.0, 50.0, 100.0, 200.0, 400.0, 800.0, 1600.0};
    return kHz[static_cast<std::uint8_t>(rate)];
}

// Datasheet: 400 kHz I2C supports 800 Hz ODR, and the limit scales linearly.
[[nodiscard]] constexpr double max_recommended_odr_hz(double i2c_clock_hz) noexcept {
    return i2c_clock_hz * (800.0 / 400000.0);
}

[[nodiscard]] constexpr bool i2c_rate_allowed(DataRate rate) noexcept {
    return static_cast<std::uint8_t>(rate) <= static_cast<std::uint8_t>(DataRate::hz800);
}

[[nodiscard]] const char* to_string(DataRate rate) noexcept;
[[nodiscard]] const char* to_string(Range range) noexcept;

[[nodiscard]] constexpr RawSample decode_le(std::span<const std::uint8_t, 6> bytes) noexcept {
    auto word = [](std::uint8_t lo, std::uint8_t hi) noexcept {
        return static_cast<std::int16_t>(
            static_cast<std::uint16_t>(lo) | (static_cast<std::uint16_t>(hi) << 8));
    };
    return RawSample{word(bytes[0], bytes[1]), word(bytes[2], bytes[3]), word(bytes[4], bytes[5])};
}

// Right-justified data is already a sign-extended int16. Left-justified data
// has the MSB in bit 15 and zeros in the unused low bits.
[[nodiscard]] constexpr RawSample normalize(RawSample raw, DataFormat format) noexcept {
    if (!format.left_justified) {
        return raw;
    }
    const int shift = 16 - resolution_bits(format.range, format.full_resolution);
    return RawSample{static_cast<std::int16_t>(raw.x >> shift),
                     static_cast<std::int16_t>(raw.y >> shift),
                     static_cast<std::int16_t>(raw.z >> shift)};
}

[[nodiscard]] inline Acceleration counts_to_g(RawSample counts, DataFormat format) noexcept {
    const double scale = 1.0 / static_cast<double>(nominal_lsb_per_g(format.range, format.full_resolution));
    return Acceleration{static_cast<double>(counts.x) * scale, static_cast<double>(counts.y) * scale,
                        static_cast<double>(counts.z) * scale};
}

// 62.5 mg/LSB unsigned thresholds (tap, activity, inactivity, free-fall).
[[nodiscard]] std::uint8_t quantize_threshold_g(double g);

// 625 µs/LSB tap duration. 1.25 ms/LSB latent and window. 5 ms/LSB free-fall time.
// 1 s/LSB inactivity time.
[[nodiscard]] std::uint8_t quantize_tap_duration_s(double seconds);
[[nodiscard]] std::uint8_t quantize_tap_window_s(double seconds);
[[nodiscard]] std::uint8_t quantize_free_fall_time_s(double seconds);
[[nodiscard]] std::uint8_t quantize_inactivity_time_s(double seconds);

// Single-point offset calibration. `ideal_counts` is the expected right-justified
// reading (for example {0, 0, 256} with Z up in full resolution). The offset
// registers are 15.6 mg/LSB, which is 4 data LSBs at 256 LSB/g, and the ratio
// changes when 10-bit mode widens the data LSB. Half-LSB ties round away from zero,
// matching the datasheet's Round(10/4) = 3 example.
[[nodiscard]] AxisOffsets compute_offsets(std::span<const RawSample> samples, RawSample ideal_counts,
                                          int lsb_per_g);

// Table 15 applies to ±2 g 10-bit and to full resolution at any range.
// Tables 16-18 apply to fixed 10-bit ±4/8/16 g. Table 14 scales those LSB
// limits from the 2.5 V characterization voltage to `supply_volts` (2.0 V to 3.6 V).
[[nodiscard]] SelfTestLimits self_test_limits(DataFormat format, double supply_volts);

[[nodiscard]] inline double magnitude_g(Acceleration sample) noexcept {
    return sample.magnitude_g();
}

static_assert(nominal_lsb_per_g(Range::g2, false) == 256);
static_assert(nominal_lsb_per_g(Range::g4, false) == 128);
static_assert(nominal_lsb_per_g(Range::g8, false) == 64);
static_assert(nominal_lsb_per_g(Range::g16, false) == 32);
static_assert(nominal_lsb_per_g(Range::g16, true) == 256);
static_assert(resolution_bits(Range::g2, true) == 10);
static_assert(resolution_bits(Range::g4, true) == 11);
static_assert(resolution_bits(Range::g8, true) == 12);
static_assert(resolution_bits(Range::g16, true) == 13);
static_assert(resolution_bits(Range::g16, false) == 10);
static_assert(encode(DataFormat{.full_resolution = true, .range = Range::g16}) == 0x0B);
static_assert(encode_bw_rate(DataRate::hz100, false) == 0x0A);
static_assert(encode_bw_rate(DataRate::hz100, true) == 0x1A);
static_assert(encode(PowerControl{.measure = true}) == reg::power_measure);
static_assert(encode(PowerControl{.link = true, .auto_sleep = true, .measure = true, .sleep = true, .wakeup = WakeupFrequency::hz1}) == 0x3F);
static_assert(encode(FifoControl{.mode = FifoMode::stream, .trigger_int2 = true, .samples = 16}) == 0xB0);
static_assert(data_rate_hz(DataRate::hz100) == 100.0);
static_assert(bandwidth_hz(DataRate::hz100) == 50.0);
static_assert(!i2c_rate_allowed(DataRate::hz1600));
static_assert(i2c_rate_allowed(DataRate::hz800));

}  // namespace adxl345
