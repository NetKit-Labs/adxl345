#pragma once

#include <cstdint>
#include <span>

namespace adxl345 {

// Byte transactions against one already-addressed I2C slave.
// `write` sends the register address followed by `data`.
// `read` writes the register address, repeats the start, and reads `data.size()` bytes.
// The ADXL345 I2C pointer auto-increments, so a 6-byte read at DATAX0 returns X, Y, and Z.
class I2cBus {
public:
    virtual ~I2cBus() = default;

    virtual void write(std::uint8_t reg, std::span<const std::uint8_t> data) = 0;
    virtual void read(std::uint8_t reg, std::span<std::uint8_t> data) = 0;
};

}  // namespace adxl345
