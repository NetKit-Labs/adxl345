#include "adxl345/codec.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace adxl345 {
namespace {

[[nodiscard]] std::uint8_t quantize(double value, double lsb, double max_value, const char* what) {
    if (!std::isfinite(value) || value < 0.0 || value > max_value + (lsb * 0.51)) {
        throw std::invalid_argument(std::string(what) + " is out of range");
    }
    long quantized = std::lround(value / lsb);
    if (quantized < 0) {
        quantized = 0;
    }
    if (quantized > 255) {
        quantized = 255;
    }
    return static_cast<std::uint8_t>(quantized);
}

[[nodiscard]] std::int32_t rounded_mean(std::int64_t sum, std::int64_t count) {
    const std::int64_t magnitude = sum >= 0 ? sum : -sum;
    const std::int64_t rounded = (magnitude + (count / 2)) / count;
    return static_cast<std::int32_t>(sum >= 0 ? rounded : -rounded);
}

}  // namespace

double Acceleration::magnitude_g() const {
    return std::sqrt((x_g * x_g) + (y_g * y_g) + (z_g * z_g));
}

const char* to_string(DataRate rate) noexcept {
    switch (rate) {
        case DataRate::hz0_10:
            return "0.10 Hz";
        case DataRate::hz0_20:
            return "0.20 Hz";
        case DataRate::hz0_39:
            return "0.39 Hz";
        case DataRate::hz0_78:
            return "0.78 Hz";
        case DataRate::hz1_56:
            return "1.56 Hz";
        case DataRate::hz3_13:
            return "3.13 Hz";
        case DataRate::hz6_25:
            return "6.25 Hz";
        case DataRate::hz12_5:
            return "12.5 Hz";
        case DataRate::hz25:
            return "25 Hz";
        case DataRate::hz50:
            return "50 Hz";
        case DataRate::hz100:
            return "100 Hz";
        case DataRate::hz200:
            return "200 Hz";
        case DataRate::hz400:
            return "400 Hz";
        case DataRate::hz800:
            return "800 Hz";
        case DataRate::hz1600:
            return "1600 Hz";
        case DataRate::hz3200:
            return "3200 Hz";
    }
    return "unknown";
}

const char* to_string(Range range) noexcept {
    switch (range) {
        case Range::g2:
            return "±2 g";
        case Range::g4:
            return "±4 g";
        case Range::g8:
            return "±8 g";
        case Range::g16:
            return "±16 g";
    }
    return "unknown";
}

std::uint8_t quantize_threshold_g(double g) {
    return quantize(g, 0.0625, 16.0, "threshold");
}

std::uint8_t quantize_tap_duration_s(double seconds) {
    return quantize(seconds, 625e-6, 255.0 * 625e-6, "tap duration");
}

std::uint8_t quantize_tap_window_s(double seconds) {
    return quantize(seconds, 1.25e-3, 255.0 * 1.25e-3, "tap latency/window");
}

std::uint8_t quantize_free_fall_time_s(double seconds) {
    return quantize(seconds, 5e-3, 255.0 * 5e-3, "free-fall time");
}

std::uint8_t quantize_inactivity_time_s(double seconds) {
    return quantize(seconds, 1.0, 255.0, "inactivity time");
}

AxisOffsets compute_offsets(std::span<const RawSample> samples, RawSample ideal_counts, int lsb_per_g) {
    if (samples.empty()) {
        throw std::invalid_argument("offset calibration needs at least one sample");
    }
    if (lsb_per_g <= 0) {
        throw std::invalid_argument("LSB/g must be positive");
    }

    std::int64_t sum_x = 0;
    std::int64_t sum_y = 0;
    std::int64_t sum_z = 0;
    for (const RawSample& sample : samples) {
        sum_x += sample.x;
        sum_y += sample.y;
        sum_z += sample.z;
    }
    const auto count = static_cast<std::int64_t>(samples.size());
    const std::int32_t mean_x = rounded_mean(sum_x, count);
    const std::int32_t mean_y = rounded_mean(sum_y, count);
    const std::int32_t mean_z = rounded_mean(sum_z, count);

    auto axis = [lsb_per_g](std::int32_t measured, std::int32_t ideal) -> std::int8_t {
        const std::int32_t error = measured - ideal;
        const std::int32_t numerator = error * 64;
        const std::int32_t denominator = lsb_per_g;
        const std::int32_t magnitude = numerator >= 0 ? numerator : -numerator;
        const std::int32_t rounded = (magnitude + (denominator / 2)) / denominator;
        const std::int32_t signed_rounded = numerator >= 0 ? rounded : -rounded;
        std::int32_t offset = -signed_rounded;
        if (offset > 127) {
            offset = 127;
        }
        if (offset < -128) {
            offset = -128;
        }
        return static_cast<std::int8_t>(offset);
    };

    return AxisOffsets{axis(mean_x, ideal_counts.x), axis(mean_y, ideal_counts.y), axis(mean_z, ideal_counts.z)};
}

SelfTestLimits self_test_limits(DataFormat format, double supply_volts) {
    if (!std::isfinite(supply_volts) || supply_volts < 2.0 || supply_volts > 3.6) {
        throw std::invalid_argument("self-test supply voltage must be from 2.0 V to 3.6 V");
    }

    struct Point {
        double volts;
        double xy;
        double z;
    };
    constexpr Point points[] = {
        {2.0, 0.64, 0.80},
        {2.5, 1.00, 1.00},
        {3.3, 1.77, 1.47},
        {3.6, 2.11, 1.69},
    };

    double xy = points[0].xy;
    double z = points[0].z;
    if (supply_volts <= points[0].volts) {
        xy = points[0].xy;
        z = points[0].z;
    } else if (supply_volts >= points[3].volts) {
        xy = points[3].xy;
        z = points[3].z;
    } else {
        for (std::size_t i = 0; i + 1 < 4; ++i) {
            if (supply_volts <= points[i + 1].volts) {
                const double span = points[i + 1].volts - points[i].volts;
                const double t = (supply_volts - points[i].volts) / span;
                xy = points[i].xy + (t * (points[i + 1].xy - points[i].xy));
                z = points[i].z + (t * (points[i + 1].z - points[i].z));
                break;
            }
        }
    }

    struct Base {
        int min_x;
        int max_x;
        int min_y;
        int max_y;
        int min_z;
        int max_z;
    };
    Base base{50, 540, -540, -50, 75, 875};
    if (!format.full_resolution && format.range == Range::g4) {
        base = Base{25, 270, -270, -25, 38, 438};
    } else if (!format.full_resolution && format.range == Range::g8) {
        base = Base{12, 135, -135, -12, 19, 219};
    } else if (!format.full_resolution && format.range == Range::g16) {
        base = Base{6, 67, -67, -6, 10, 110};
    }

    auto scale = [](int limit, double factor) {
        return static_cast<int>(std::lround(static_cast<double>(limit) * factor));
    };
    return SelfTestLimits{scale(base.min_x, xy), scale(base.max_x, xy), scale(base.min_y, xy),
                          scale(base.max_y, xy), scale(base.min_z, z), scale(base.max_z, z)};
}

}  // namespace adxl345
