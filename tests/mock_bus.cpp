#include "mock_bus.hpp"

#include "adxl345/codec.hpp"
#include "adxl345/registers.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace adxl345::testutil {
namespace {

[[nodiscard]] std::int16_t sat_add(std::int16_t left, std::int16_t right) {
    const int sum = static_cast<int>(left) + static_cast<int>(right);
    if (sum > 32767) {
        return 32767;
    }
    if (sum < -32768) {
        return static_cast<std::int16_t>(-32768);
    }
    return static_cast<std::int16_t>(sum);
}

[[nodiscard]] std::int16_t shift_left(std::int16_t value, int shift) {
    return static_cast<std::int16_t>(static_cast<int>(value) << shift);
}

}  // namespace

MockBus::MockBus() {
    regs_[reg::devid] = reg::device_id;
    regs_[reg::bw_rate] = 0x0A;
    regs_[reg::int_source] = 0x02;
}

bool MockBus::writable(std::uint8_t address) const noexcept {
    const bool feature = address >= reg::thresh_tap && address <= reg::tap_axes;
    const bool control = address >= reg::bw_rate && address <= reg::int_map;
    return feature || control || address == reg::data_format || address == reg::fifo_ctl;
}

bool MockBus::measuring() const noexcept {
    return (regs_[reg::power_ctl] & reg::power_measure) != 0;
}

RawSample MockBus::presented_counts() const {
    if (!measuring()) {
        return {};
    }
    RawSample sample = sticky_;
    if ((regs_[reg::data_format] & reg::fmt_self_test) != 0) {
        sample.x = sat_add(sample.x, delta_.x);
        sample.y = sat_add(sample.y, delta_.y);
        sample.z = sat_add(sample.z, delta_.z);
    }
    return sample;
}

void MockBus::refresh_data() {
    RawSample sample = presented_counts();
    const DataFormat format = decode_data_format(regs_[reg::data_format]);
    if (format.left_justified) {
        const int shift = 16 - resolution_bits(format.range, format.full_resolution);
        sample.x = shift_left(sample.x, shift);
        sample.y = shift_left(sample.y, shift);
        sample.z = shift_left(sample.z, shift);
    }
    auto store = [this](std::uint8_t lo, std::uint8_t hi, std::int16_t value) {
        const auto bits = static_cast<std::uint16_t>(value);
        regs_[lo] = static_cast<std::uint8_t>(bits & 0xFFu);
        regs_[hi] = static_cast<std::uint8_t>((bits >> 8) & 0xFFu);
    };
    store(reg::data_x0, reg::data_x1, sample.x);
    store(reg::data_y0, reg::data_y1, sample.y);
    store(reg::data_z0, reg::data_z1, sample.z);
}

void MockBus::clear_latched_data_interrupts() {
    constexpr std::uint8_t kClear = static_cast<std::uint8_t>(reg::irq::data_ready | reg::irq::watermark | reg::irq::overrun);
    regs_[reg::int_source] = static_cast<std::uint8_t>(regs_[reg::int_source] & static_cast<std::uint8_t>(~kClear));
}

void MockBus::set_sample(RawSample sample) {
    sticky_ = sample;
    if (measuring()) {
        refresh_data();
        regs_[reg::int_source] = static_cast<std::uint8_t>(regs_[reg::int_source] | reg::irq::data_ready);
    }
}

void MockBus::set_self_test_delta(RawSample delta) {
    delta_ = delta;
}

void MockBus::set_device_id(std::uint8_t id) {
    regs_[reg::devid] = id;
}

void MockBus::set_reg(std::uint8_t address, std::uint8_t value) {
    if (address >= sizeof(regs_)) {
        throw std::out_of_range("mock register");
    }
    regs_[address] = value;
}

std::uint8_t MockBus::reg(std::uint8_t address) const {
    if (address >= sizeof(regs_)) {
        throw std::out_of_range("mock register");
    }
    return regs_[address];
}

void MockBus::fail_next(int err) {
    fail_errno_ = err == 0 ? EIO : err;
}

void MockBus::write(std::uint8_t reg_addr, std::span<const std::uint8_t> data) {
    if (fail_errno_ != 0) {
        const int err = fail_errno_;
        fail_errno_ = 0;
        throw std::system_error(err, std::generic_category(), "mock I2C write");
    }
    if (data.empty()) {
        throw std::invalid_argument("empty write");
    }
    log_.push_back(Xfer{XferKind::write, reg_addr, std::vector<std::uint8_t>(data.begin(), data.end())});
    for (std::size_t i = 0; i < data.size(); ++i) {
        const auto address = static_cast<std::uint8_t>(reg_addr + i);
        if (address < sizeof(regs_) && writable(address)) {
            regs_[address] = data[i];
        }
    }
}

void MockBus::read(std::uint8_t reg_addr, std::span<std::uint8_t> data) {
    if (fail_errno_ != 0) {
        const int err = fail_errno_;
        fail_errno_ = 0;
        throw std::system_error(err, std::generic_category(), "mock I2C read");
    }
    if (data.empty()) {
        throw std::invalid_argument("empty read");
    }
    const unsigned last = static_cast<unsigned>(reg_addr) + static_cast<unsigned>(data.size()) - 1u;
    const bool overlaps_data = reg_addr <= reg::data_z1 && last >= reg::data_x0;
    if (overlaps_data) {
        refresh_data();
    }
    for (std::size_t i = 0; i < data.size(); ++i) {
        const auto address = static_cast<unsigned>(reg_addr) + static_cast<unsigned>(i);
        if (address >= sizeof(regs_)) {
            data[i] = 0;
            continue;
        }
        const auto index = static_cast<std::uint8_t>(address);
        data[i] = regs_[index];
        if (index == reg::int_source) {
            regs_[index] = static_cast<std::uint8_t>(regs_[index] & static_cast<std::uint8_t>(~reg::int_source_clear_on_read));
        }
    }
    if (reg_addr <= reg::data_x0 && last >= reg::data_z1) {
        clear_latched_data_interrupts();
    }
    log_.push_back(Xfer{XferKind::read, reg_addr, std::vector<std::uint8_t>(data.begin(), data.end())});
}

}  // namespace adxl345::testutil
