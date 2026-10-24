#pragma once

#ifdef __linux__

#include "adxl345/i2c_bus.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace adxl345 {

// Opens `/dev/i2c-N` and talks to one 7-bit slave with I2C_RDWR (repeated start).
class LinuxI2cBus final : public I2cBus {
public:
    LinuxI2cBus(std::string device_path, std::uint8_t address);
    ~LinuxI2cBus() override;

    LinuxI2cBus(const LinuxI2cBus&) = delete;
    LinuxI2cBus& operator=(const LinuxI2cBus&) = delete;
    LinuxI2cBus(LinuxI2cBus&& other) noexcept;
    LinuxI2cBus& operator=(LinuxI2cBus&& other) noexcept;

    void write(std::uint8_t reg, std::span<const std::uint8_t> data) override;
    void read(std::uint8_t reg, std::span<std::uint8_t> data) override;

    [[nodiscard]] const std::string& device_path() const noexcept { return path_; }
    [[nodiscard]] std::uint8_t address() const noexcept { return address_; }

private:
    void close_fd() noexcept;

    std::string path_;
    std::uint8_t address_ = 0;
    int fd_ = -1;
};

// Tries 0x53, then 0x1D. Returns the address whose DEVID register is 0xE5.
[[nodiscard]] std::optional<std::uint8_t> probe_adxl345(const std::string& device_path);

}  // namespace adxl345

#endif
