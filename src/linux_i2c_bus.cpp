#include "adxl345/linux_i2c_bus.hpp"

#include "adxl345/registers.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace adxl345 {
namespace {

constexpr std::size_t kMaxPayload = 32;

void transfer(int fd, i2c_msg* messages, std::uint32_t count, const char* what) {
    i2c_rdwr_ioctl_data ioctl_data{};
    ioctl_data.msgs = messages;
    ioctl_data.nmsgs = count;
    if (::ioctl(fd, I2C_RDWR, &ioctl_data) < 0) {
        throw std::system_error(errno, std::generic_category(), what);
    }
}

}  // namespace

LinuxI2cBus::LinuxI2cBus(std::string device_path, std::uint8_t address)
    : path_(std::move(device_path)), address_(address) {
    if (address_ > 0x7F) {
        throw std::invalid_argument("I2C address must be a 7-bit value");
    }
    fd_ = ::open(path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open " + path_);
    }
}

LinuxI2cBus::~LinuxI2cBus() {
    close_fd();
}

LinuxI2cBus::LinuxI2cBus(LinuxI2cBus&& other) noexcept
    : path_(std::move(other.path_)), address_(other.address_), fd_(std::exchange(other.fd_, -1)) {}

LinuxI2cBus& LinuxI2cBus::operator=(LinuxI2cBus&& other) noexcept {
    if (this != &other) {
        close_fd();
        path_ = std::move(other.path_);
        address_ = other.address_;
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void LinuxI2cBus::close_fd() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void LinuxI2cBus::write(std::uint8_t reg, std::span<const std::uint8_t> data) {
    if (data.empty() || data.size() > kMaxPayload) {
        throw std::invalid_argument("I2C write payload must be 1 to 32 bytes");
    }
    std::uint8_t buffer[kMaxPayload + 1] = {};
    buffer[0] = reg;
    std::copy(data.begin(), data.end(), buffer + 1);

    i2c_msg message{};
    message.addr = address_;
    message.flags = 0;
    message.len = static_cast<__u16>(data.size() + 1);
    message.buf = buffer;
    transfer(fd_, &message, 1, "I2C write");
}

void LinuxI2cBus::read(std::uint8_t reg, std::span<std::uint8_t> data) {
    if (data.empty() || data.size() > kMaxPayload) {
        throw std::invalid_argument("I2C read length must be 1 to 32 bytes");
    }
    // I2C uses the register address as-is. Bit 6 is the SPI multiple-byte flag,
    // not part of the I2C pointer. The pointer increments on its own (Figure 41).
    std::uint8_t pointer = reg;
    i2c_msg messages[2] = {};
    messages[0].addr = address_;
    messages[0].flags = 0;
    messages[0].len = 1;
    messages[0].buf = &pointer;
    messages[1].addr = address_;
    messages[1].flags = I2C_M_RD;
    messages[1].len = static_cast<__u16>(data.size());
    messages[1].buf = data.data();
    transfer(fd_, messages, 2, "I2C read");
}

std::optional<std::uint8_t> probe_adxl345(const std::string& device_path) {
    const std::uint8_t candidates[] = {reg::address_sdo_low, reg::address_sdo_high};
    for (const std::uint8_t address : candidates) {
        try {
            LinuxI2cBus bus(device_path, address);
            std::uint8_t id = 0;
            bus.read(reg::devid, std::span<std::uint8_t>(&id, 1));
            if (id == reg::device_id) {
                return address;
            }
        } catch (const std::system_error&) {
        }
    }
    return std::nullopt;
}

}  // namespace adxl345
