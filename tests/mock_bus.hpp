#pragma once

#include "adxl345/i2c_bus.hpp"
#include "adxl345/types.hpp"

#include <cstdint>
#include <vector>

namespace adxl345::testutil {

enum class XferKind { read, write };

struct Xfer {
    XferKind kind = XferKind::read;
    std::uint8_t reg = 0;
    std::vector<std::uint8_t> data;
};

// Register file with ADXL345 reset defaults. Samples programmed with set_sample()
// are right-justified counts. The mock shifts them when DATA_FORMAT is left-justified
// and adds the self-test delta while that bit is set. Standby reads back as zero.
class MockBus final : public I2cBus {
public:
    MockBus();

    void write(std::uint8_t reg, std::span<const std::uint8_t> data) override;
    void read(std::uint8_t reg, std::span<std::uint8_t> data) override;

    void set_sample(RawSample sample);
    void set_self_test_delta(RawSample delta);
    void set_device_id(std::uint8_t id);
    void set_reg(std::uint8_t address, std::uint8_t value);
    void fail_next(int err);

    [[nodiscard]] std::uint8_t reg(std::uint8_t address) const;
    [[nodiscard]] const std::vector<Xfer>& log() const noexcept { return log_; }
    void clear_log() { log_.clear(); }

private:
    [[nodiscard]] bool writable(std::uint8_t address) const noexcept;
    [[nodiscard]] bool measuring() const noexcept;
    [[nodiscard]] RawSample presented_counts() const;
    void refresh_data();
    void clear_latched_data_interrupts();

    std::uint8_t regs_[0x3A] = {};
    RawSample sticky_{};
    RawSample delta_{};
    std::vector<Xfer> log_;
    int fail_errno_ = 0;
};

}  // namespace adxl345::testutil
