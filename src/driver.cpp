#include "adxl345/driver.hpp"

#include <cmath>
#include <format>
#include <stdexcept>
#include <thread>
#include <utility>

namespace adxl345 {
namespace {

struct SelfTestRestore {
    Adxl345& device;
    PowerControl power;
    DataFormat format;
    DataRate rate;
    bool low_power;

    ~SelfTestRestore() {
        try {
            device.set_data_format(format);
            device.set_data_rate(rate, low_power, HighRatePolicy::allow);
            device.set_power(power);
        } catch (...) {
        }
    }
};

}  // namespace

Adxl345::Adxl345(I2cBus& bus) : bus_(bus) {}

void Adxl345::write8(std::uint8_t reg, std::uint8_t value) {
    bus_.write(reg, std::span<const std::uint8_t>(&value, 1));
}

std::uint8_t Adxl345::read8(std::uint8_t reg) {
    std::uint8_t value = 0;
    bus_.read(reg, std::span<std::uint8_t>(&value, 1));
    return value;
}

void Adxl345::verify() {
    const std::uint8_t id = device_id();
    if (id != reg::device_id) {
        throw Error(Error::Code::bad_device_id,
                    std::format("ADXL345 DEVID was 0x{:02X}, expected 0xE5", id));
    }
}

std::uint8_t Adxl345::device_id() {
    return read8(reg::devid);
}

void Adxl345::set_data_format(DataFormat format) {
    write8(reg::data_format, encode(format));
}

DataFormat Adxl345::data_format() {
    return decode_data_format(read8(reg::data_format));
}

void Adxl345::set_data_rate(DataRate rate, bool low_power, HighRatePolicy policy) {
    if (policy == HighRatePolicy::reject_above_800hz && !i2c_rate_allowed(rate)) {
        throw std::invalid_argument(
            "1600 Hz and 3200 Hz are not recommended ADXL345 output data rates on I2C");
    }
    write8(reg::bw_rate, encode_bw_rate(rate, low_power));
}

DataRate Adxl345::data_rate() {
    return static_cast<DataRate>(read8(reg::bw_rate) & reg::bw_rate_mask);
}

bool Adxl345::low_power() {
    return (read8(reg::bw_rate) & reg::bw_low_power) != 0;
}

void Adxl345::set_power(PowerControl desired) {
    const PowerControl current = power();
    const bool standby_first = (current.sleep && !desired.sleep) || (current.link && !desired.link) ||
                               (current.auto_sleep && !desired.auto_sleep);
    if (standby_first) {
        PowerControl standby = desired;
        standby.measure = false;
        write8(reg::power_ctl, encode(standby));
        if (desired.measure) {
            write8(reg::power_ctl, encode(desired));
        }
        return;
    }
    write8(reg::power_ctl, encode(desired));
}

PowerControl Adxl345::power() {
    return decode_power(read8(reg::power_ctl));
}

void Adxl345::start() {
    PowerControl next = power();
    next.measure = true;
    set_power(next);
}

void Adxl345::standby() {
    PowerControl next = power();
    next.measure = false;
    set_power(next);
}

bool Adxl345::measuring() {
    return power().measure;
}

void Adxl345::set_offsets(AxisOffsets offsets) {
    const std::uint8_t bytes[3] = {static_cast<std::uint8_t>(offsets.x), static_cast<std::uint8_t>(offsets.y),
                                   static_cast<std::uint8_t>(offsets.z)};
    bus_.write(reg::ofsx, bytes);
}

AxisOffsets Adxl345::offsets() {
    std::uint8_t bytes[3] = {};
    bus_.read(reg::ofsx, bytes);
    return AxisOffsets{static_cast<std::int8_t>(bytes[0]), static_cast<std::int8_t>(bytes[1]),
                       static_cast<std::int8_t>(bytes[2])};
}

void Adxl345::set_interrupts_enabled(std::uint8_t mask) {
    write8(reg::int_enable, mask);
}

void Adxl345::map_interrupts(std::uint8_t int2_mask) {
    write8(reg::int_map, int2_mask);
}

std::uint8_t Adxl345::interrupt_source() {
    return read8(reg::int_source);
}

ActTapStatus Adxl345::activity_tap_status() {
    return decode_act_tap_status(read8(reg::act_tap_status));
}

void Adxl345::configure_tap(const TapSettings& settings) {
    write8(reg::thresh_tap, quantize_threshold_g(settings.threshold_g));
    write8(reg::dur, quantize_tap_duration_s(settings.duration_s));
    write8(reg::latent, quantize_tap_window_s(settings.latency_s));
    write8(reg::window, quantize_tap_window_s(settings.window_s));
    std::uint8_t axes = 0;
    if (settings.suppress) {
        axes = static_cast<std::uint8_t>(axes | reg::tap_suppress);
    }
    if (settings.x) {
        axes = static_cast<std::uint8_t>(axes | reg::tap_x);
    }
    if (settings.y) {
        axes = static_cast<std::uint8_t>(axes | reg::tap_y);
    }
    if (settings.z) {
        axes = static_cast<std::uint8_t>(axes | reg::tap_z);
    }
    write8(reg::tap_axes, axes);
}

void Adxl345::configure_activity(const ActivitySettings& settings) {
    write8(reg::thresh_act, quantize_threshold_g(settings.activity_threshold_g));
    write8(reg::thresh_inact, quantize_threshold_g(settings.inactivity_threshold_g));
    write8(reg::time_inact, quantize_inactivity_time_s(settings.inactivity_time_s));
    std::uint8_t control = 0;
    if (settings.activity_ac) {
        control = static_cast<std::uint8_t>(control | reg::act_ac_dc);
    }
    if (settings.activity_x) {
        control = static_cast<std::uint8_t>(control | reg::act_x);
    }
    if (settings.activity_y) {
        control = static_cast<std::uint8_t>(control | reg::act_y);
    }
    if (settings.activity_z) {
        control = static_cast<std::uint8_t>(control | reg::act_z);
    }
    if (settings.inactivity_ac) {
        control = static_cast<std::uint8_t>(control | reg::inact_ac_dc);
    }
    if (settings.inactivity_x) {
        control = static_cast<std::uint8_t>(control | reg::inact_x);
    }
    if (settings.inactivity_y) {
        control = static_cast<std::uint8_t>(control | reg::inact_y);
    }
    if (settings.inactivity_z) {
        control = static_cast<std::uint8_t>(control | reg::inact_z);
    }
    write8(reg::act_inact_ctl, control);
}

void Adxl345::configure_free_fall(const FreeFallSettings& settings) {
    write8(reg::thresh_ff, quantize_threshold_g(settings.threshold_g));
    write8(reg::time_ff, quantize_free_fall_time_s(settings.time_s));
}

void Adxl345::set_fifo(FifoControl fifo) {
    if (fifo.samples > reg::fifo_samples_mask) {
        throw std::invalid_argument("FIFO samples field is 5 bits (0 through 31)");
    }
    write8(reg::fifo_ctl, encode(fifo));
}

FifoStatus Adxl345::fifo_status() {
    return decode_fifo_status(read8(reg::fifo_status));
}

void Adxl345::set_self_test(bool enabled) {
    DataFormat format = data_format();
    format.self_test = enabled;
    set_data_format(format);
}

RawSample Adxl345::read_raw() {
    std::uint8_t bytes[6] = {};
    bus_.read(reg::data_x0, bytes);
    return decode_le(std::span<const std::uint8_t, 6>{bytes});
}

Acceleration Adxl345::read_g() {
    const DataFormat format = data_format();
    return counts_to_g(normalize(read_raw(), format), format);
}

RawSample Adxl345::read_raw_when_ready(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if ((interrupt_source() & reg::irq::data_ready) != 0) {
            return read_raw();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw Error(Error::Code::timeout, "timed out waiting for DATA_READY");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

Acceleration Adxl345::read_g_when_ready(std::chrono::milliseconds timeout) {
    const DataFormat format = data_format();
    return counts_to_g(normalize(read_raw_when_ready(timeout), format), format);
}

RawSample Adxl345::average_samples(int samples) {
    std::int64_t sum_x = 0;
    std::int64_t sum_y = 0;
    std::int64_t sum_z = 0;
    for (int i = 0; i < samples; ++i) {
        const RawSample sample = read_raw();
        sum_x += sample.x;
        sum_y += sample.y;
        sum_z += sample.z;
    }
    const auto count = static_cast<std::int64_t>(samples);
    auto mean = [count](std::int64_t sum) -> std::int16_t {
        const std::int64_t magnitude = sum >= 0 ? sum : -sum;
        const std::int64_t rounded = (magnitude + (count / 2)) / count;
        const auto signed_rounded = static_cast<std::int32_t>(sum >= 0 ? rounded : -rounded);
        return static_cast<std::int16_t>(signed_rounded);
    };
    return RawSample{mean(sum_x), mean(sum_y), mean(sum_z)};
}

AxisOffsets Adxl345::calibrate(int samples, RawSample ideal_counts) {
    if (samples <= 0) {
        throw std::invalid_argument("calibration needs at least one sample");
    }
    set_offsets(AxisOffsets{});
    const DataFormat format = data_format();
    std::int64_t sum_x = 0;
    std::int64_t sum_y = 0;
    std::int64_t sum_z = 0;
    for (int i = 0; i < samples; ++i) {
        const RawSample sample = normalize(read_raw(), format);
        sum_x += sample.x;
        sum_y += sample.y;
        sum_z += sample.z;
    }
    // compute_offsets averages again. Hand it one already-averaged sample so the
    // rounding matches a single mean, including the datasheet's worked example.
    const auto count = static_cast<std::int64_t>(samples);
    auto mean = [count](std::int64_t sum) -> std::int16_t {
        const std::int64_t magnitude = sum >= 0 ? sum : -sum;
        const std::int64_t rounded = (magnitude + (count / 2)) / count;
        return static_cast<std::int16_t>(sum >= 0 ? rounded : -rounded);
    };
    const RawSample averaged{mean(sum_x), mean(sum_y), mean(sum_z)};
    const AxisOffsets computed =
        compute_offsets(std::span<const RawSample>(&averaged, 1), ideal_counts,
                        nominal_lsb_per_g(format.range, format.full_resolution));
    set_offsets(computed);
    return computed;
}

SelfTestResult Adxl345::run_self_test(SelfTestOptions options) {
    if (options.samples <= 0 || options.discard < 0 || options.settle_ms < 0) {
        throw std::invalid_argument("self-test sample counts must be non-negative, with at least one averaged sample");
    }
    if (!std::isfinite(options.supply_volts) || options.supply_volts < 2.0 || options.supply_volts > 3.6) {
        throw std::invalid_argument("self-test supply voltage must be from 2.0 V to 3.6 V");
    }

    const SelfTestRestore restore{*this, power(), data_format(), data_rate(), low_power()};

    DataFormat test_format = restore.format;
    test_format.self_test = false;
    test_format.spi_3wire = false;
    test_format.full_resolution = true;
    test_format.left_justified = false;
    test_format.range = Range::g16;
    set_data_format(test_format);
    set_data_rate(DataRate::hz100, false);

    PowerControl measuring = restore.power;
    measuring.measure = true;
    measuring.sleep = false;
    set_power(measuring);

    const auto settle = std::chrono::milliseconds(options.settle_ms);
    if (options.settle_ms > 0) {
        std::this_thread::sleep_for(settle);
    }
    if (options.discard > 0) {
        (void)average_samples(options.discard);
    }
    const RawSample off = average_samples(options.samples);

    set_self_test(true);
    if (options.settle_ms > 0) {
        std::this_thread::sleep_for(settle);
    }
    if (options.discard > 0) {
        (void)average_samples(options.discard);
    }
    const RawSample on = average_samples(options.samples);

    SelfTestResult result;
    result.off = off;
    result.on = on;
    result.x_lsb = static_cast<std::int32_t>(on.x) - static_cast<std::int32_t>(off.x);
    result.y_lsb = static_cast<std::int32_t>(on.y) - static_cast<std::int32_t>(off.y);
    result.z_lsb = static_cast<std::int32_t>(on.z) - static_cast<std::int32_t>(off.z);
    result.x_g = static_cast<double>(result.x_lsb) / 256.0;
    result.y_g = static_cast<double>(result.y_lsb) / 256.0;
    result.z_g = static_cast<double>(result.z_lsb) / 256.0;
    result.limits = self_test_limits(test_format, options.supply_volts);
    result.within_window = result.x_lsb >= result.limits.min_x && result.x_lsb <= result.limits.max_x &&
                           result.y_lsb >= result.limits.min_y && result.y_lsb <= result.limits.max_y &&
                           result.z_lsb >= result.limits.min_z && result.z_lsb <= result.limits.max_z;
    result.meets_minimum = result.x_lsb >= result.limits.min_x && result.y_lsb <= result.limits.max_y &&
                           result.z_lsb >= result.limits.min_z;
    return result;
}

std::uint8_t Adxl345::read_register(std::uint8_t reg) {
    return read8(reg);
}

void Adxl345::write_register(std::uint8_t reg, std::uint8_t value) {
    write8(reg, value);
}

}  // namespace adxl345
