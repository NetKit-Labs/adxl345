#include "harness.hpp"
#include "mock_bus.hpp"

#include "adxl345/custom_mod_abi.h"
#include "adxl345/driver.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace {

[[nodiscard]] const adxl345::testutil::Xfer* last_write(const adxl345::testutil::MockBus& bus, std::uint8_t reg) {
    const adxl345::testutil::Xfer* found = nullptr;
    for (const adxl345::testutil::Xfer& xfer : bus.log()) {
        if (xfer.kind == adxl345::testutil::XferKind::write && xfer.reg == reg) {
            found = &xfer;
        }
    }
    return found;
}

[[nodiscard]] int write_count(const adxl345::testutil::MockBus& bus, std::uint8_t reg) {
    int count = 0;
    for (const adxl345::testutil::Xfer& xfer : bus.log()) {
        if (xfer.kind == adxl345::testutil::XferKind::write && xfer.reg == reg) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] std::vector<std::uint8_t> write_values(const adxl345::testutil::MockBus& bus, std::uint8_t reg) {
    std::vector<std::uint8_t> values;
    for (const adxl345::testutil::Xfer& xfer : bus.log()) {
        if (xfer.kind == adxl345::testutil::XferKind::write && xfer.reg == reg && !xfer.data.empty()) {
            values.push_back(xfer.data[0]);
        }
    }
    return values;
}

}  // namespace

void run_regression_tests() {
    test::Suite suite{"regression"};

    suite.test("register addresses stay on the datasheet map", [] {
        CHECK_EQ(adxl345::reg::devid, 0x00);
        CHECK_EQ(adxl345::reg::thresh_tap, 0x1D);
        CHECK_EQ(adxl345::reg::ofsx, 0x1E);
        CHECK_EQ(adxl345::reg::ofsy, 0x1F);
        CHECK_EQ(adxl345::reg::ofsz, 0x20);
        CHECK_EQ(adxl345::reg::dur, 0x21);
        CHECK_EQ(adxl345::reg::latent, 0x22);
        CHECK_EQ(adxl345::reg::window, 0x23);
        CHECK_EQ(adxl345::reg::thresh_act, 0x24);
        CHECK_EQ(adxl345::reg::thresh_inact, 0x25);
        CHECK_EQ(adxl345::reg::time_inact, 0x26);
        CHECK_EQ(adxl345::reg::act_inact_ctl, 0x27);
        CHECK_EQ(adxl345::reg::thresh_ff, 0x28);
        CHECK_EQ(adxl345::reg::time_ff, 0x29);
        CHECK_EQ(adxl345::reg::tap_axes, 0x2A);
        CHECK_EQ(adxl345::reg::act_tap_status, 0x2B);
        CHECK_EQ(adxl345::reg::bw_rate, 0x2C);
        CHECK_EQ(adxl345::reg::power_ctl, 0x2D);
        CHECK_EQ(adxl345::reg::int_enable, 0x2E);
        CHECK_EQ(adxl345::reg::int_map, 0x2F);
        CHECK_EQ(adxl345::reg::int_source, 0x30);
        CHECK_EQ(adxl345::reg::data_format, 0x31);
        CHECK_EQ(adxl345::reg::data_x0, 0x32);
        CHECK_EQ(adxl345::reg::data_z1, 0x37);
        CHECK_EQ(adxl345::reg::fifo_ctl, 0x38);
        CHECK_EQ(adxl345::reg::fifo_status, 0x39);
        CHECK_EQ(adxl345::reg::device_id, 0xE5);
        CHECK_EQ(adxl345::reg::address_sdo_low, 0x53);
        CHECK_EQ(adxl345::reg::address_sdo_high, 0x1D);
        CHECK_EQ(adxl345::reg::power_measure, 0x08);
        CHECK_EQ(adxl345::reg::bw_low_power, 0x10);
        CHECK_EQ(adxl345::reg::fmt_full_res, 0x08);
        CHECK_EQ(adxl345::reg::fmt_self_test, 0x80);
    });

    suite.test("verify accepts 0xE5 and a sample is one six-byte read at 0x32", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        device.verify();
        device.start();
        bus.set_sample(adxl345::RawSample{0x1234, -2, 7});
        bus.clear_log();
        const adxl345::RawSample raw = device.read_raw();
        CHECK_EQ(raw.x, 0x1234);
        CHECK_EQ(raw.y, -2);
        CHECK_EQ(raw.z, 7);
        CHECK_EQ(bus.log().size(), 1u);
        CHECK(bus.log()[0].kind == adxl345::testutil::XferKind::read);
        CHECK_EQ(bus.log()[0].reg, 0x32);
        CHECK_EQ(bus.log()[0].data.size(), 6u);
        CHECK(bus.log()[0].reg != 0x72);
    });

    suite.test("start sets only the measure bit and keeps link", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        device.set_power(adxl345::PowerControl{.link = true});
        bus.clear_log();
        device.start();
        const std::vector<std::uint8_t> writes = write_values(bus, adxl345::reg::power_ctl);
        CHECK_EQ(writes.size(), 1u);
        CHECK_EQ(writes[0], 0x28);
        CHECK(device.measuring());
        device.standby();
        CHECK(!device.measuring());
        CHECK_EQ(bus.reg(adxl345::reg::power_ctl), adxl345::reg::power_link);
    });

    suite.test("leaving sleep or link passes through standby first", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        device.set_power(adxl345::PowerControl{.measure = true, .sleep = true, .wakeup = adxl345::WakeupFrequency::hz4});
        CHECK_EQ(bus.reg(adxl345::reg::power_ctl), 0x0D);
        bus.clear_log();
        device.set_power(adxl345::PowerControl{.measure = true, .wakeup = adxl345::WakeupFrequency::hz4});
        const std::vector<std::uint8_t> writes = write_values(bus, adxl345::reg::power_ctl);
        CHECK_EQ(writes.size(), 2u);
        CHECK_EQ(writes[0], 0x01);
        CHECK_EQ(writes[1], 0x09);

        bus.clear_log();
        device.set_power(adxl345::PowerControl{.link = true, .auto_sleep = true, .measure = true});
        device.set_power(adxl345::PowerControl{.link = true, .measure = true});
        const std::vector<std::uint8_t> cleared = write_values(bus, adxl345::reg::power_ctl);
        CHECK_EQ(cleared.back(), 0x28);
        CHECK_EQ(cleared[cleared.size() - 2], 0x20);
    });

    suite.test("full-resolution 16 g and 100 Hz low-power encodings", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        device.set_data_format(adxl345::DataFormat{.full_resolution = true, .range = adxl345::Range::g16});
        CHECK_EQ(bus.reg(adxl345::reg::data_format), 0x0B);
        device.set_data_rate(adxl345::DataRate::hz100, true);
        CHECK_EQ(bus.reg(adxl345::reg::bw_rate), 0x1A);
        CHECK(device.low_power());
        CHECK(device.data_rate() == adxl345::DataRate::hz100);

        bool rejected = false;
        try {
            device.set_data_rate(adxl345::DataRate::hz3200, false);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK_EQ(bus.reg(adxl345::reg::bw_rate), 0x1A);

        device.set_data_rate(adxl345::DataRate::hz800, false, adxl345::HighRatePolicy::allow);
        device.set_data_rate(adxl345::DataRate::hz3200, false, adxl345::HighRatePolicy::allow);
        CHECK_EQ(bus.reg(adxl345::reg::bw_rate), 0x0F);
    });

    suite.test("left-justified samples convert back to g", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        device.start();
        device.set_data_format(
            adxl345::DataFormat{.full_resolution = false, .left_justified = true, .range = adxl345::Range::g2});
        bus.set_sample(adxl345::RawSample{1, -1, 256});
        const adxl345::RawSample raw = device.read_raw();
        CHECK_EQ(raw.x, 0x40);
        CHECK_EQ(raw.y, static_cast<std::int16_t>(0xFFC0));
        CHECK_EQ(raw.z, 0x4000);
        const adxl345::Acceleration g = device.read_g();
        CHECK_NEAR(g.x_g, 1.0 / 256.0, 1e-12);
        CHECK_NEAR(g.y_g, -1.0 / 256.0, 1e-12);
        CHECK_NEAR(g.z_g, 1.0, 1e-12);
    });

    suite.test("offset calibration programs the datasheet bytes", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        device.start();
        device.set_data_format(adxl345::DataFormat{.full_resolution = true, .range = adxl345::Range::g2});
        bus.set_sample(adxl345::RawSample{10, -13, 9});
        const adxl345::AxisOffsets offsets = device.calibrate(8, {});
        CHECK_EQ(offsets.x, -3);
        CHECK_EQ(offsets.y, 3);
        CHECK_EQ(offsets.z, -2);
        const adxl345::testutil::Xfer* write = last_write(bus, adxl345::reg::ofsx);
        CHECK(write != nullptr);
        CHECK_EQ(write->data.size(), 3u);
        CHECK_EQ(write->data[0], 0xFD);
        CHECK_EQ(write->data[1], 0x03);
        CHECK_EQ(write->data[2], 0xFE);
        const adxl345::AxisOffsets read_back = device.offsets();
        CHECK(read_back == offsets);
    });

    suite.test("tap activity freefall and fifo register images", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        device.configure_tap({});
        CHECK_EQ(bus.reg(adxl345::reg::thresh_tap), 48);
        CHECK_EQ(bus.reg(adxl345::reg::dur), 16);
        CHECK_EQ(bus.reg(adxl345::reg::latent), 16);
        CHECK_EQ(bus.reg(adxl345::reg::window), 64);
        CHECK_EQ(bus.reg(adxl345::reg::tap_axes), 0x07);

        device.configure_activity({});
        CHECK_EQ(bus.reg(adxl345::reg::thresh_act), 8);
        CHECK_EQ(bus.reg(adxl345::reg::thresh_inact), 8);
        CHECK_EQ(bus.reg(adxl345::reg::time_inact), 1);
        CHECK_EQ(bus.reg(adxl345::reg::act_inact_ctl), 0x77);

        device.configure_free_fall({});
        CHECK_EQ(bus.reg(adxl345::reg::thresh_ff), 7);
        CHECK_EQ(bus.reg(adxl345::reg::time_ff), 32);

        bool rejected = false;
        try {
            device.set_fifo(adxl345::FifoControl{.mode = adxl345::FifoMode::trigger, .samples = 32});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK_EQ(write_count(bus, adxl345::reg::fifo_ctl), 0);

        device.set_fifo(adxl345::FifoControl{.mode = adxl345::FifoMode::stream, .trigger_int2 = true, .samples = 16});
        CHECK_EQ(bus.reg(adxl345::reg::fifo_ctl), 0xB0);
        bus.set_reg(adxl345::reg::fifo_status, 0x90);
        const adxl345::FifoStatus status = device.fifo_status();
        CHECK(status.triggered);
        CHECK_EQ(status.entries, 16);
    });

    suite.test("interrupt enable map and source clear rules", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        device.set_interrupts_enabled(static_cast<std::uint8_t>(adxl345::reg::irq::data_ready | adxl345::reg::irq::overrun));
        device.map_interrupts(adxl345::reg::irq::single_tap);
        CHECK_EQ(bus.reg(adxl345::reg::int_enable), 0x81);
        CHECK_EQ(bus.reg(adxl345::reg::int_map), 0x40);

        bus.set_reg(adxl345::reg::int_source, 0xFF);
        CHECK_EQ(device.interrupt_source(), 0xFF);
        CHECK_EQ(bus.reg(adxl345::reg::int_source), 0x83);

        device.start();
        bus.set_sample(adxl345::RawSample{1, 2, 3});
        (void)device.read_raw();
        CHECK_EQ(bus.reg(adxl345::reg::int_source) & adxl345::reg::irq::data_ready, 0);
    });

    suite.test("DATA_READY poll returns once and then times out", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        device.start();
        bool timed_out = false;
        try {
            (void)device.read_raw_when_ready(std::chrono::milliseconds(0));
        } catch (const adxl345::Error& error) {
            timed_out = error.code() == adxl345::Error::Code::timeout;
        }
        CHECK(timed_out);

        bus.set_sample(adxl345::RawSample{9, 8, 7});
        const adxl345::RawSample ready = device.read_raw_when_ready(std::chrono::milliseconds(0));
        CHECK_EQ(ready.x, 9);
        CHECK_EQ(ready.z, 7);
    });

    suite.test("self-test delta, limits, and full restore", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        bus.set_reg(adxl345::reg::bw_rate, 0x0A);
        bus.set_sample(adxl345::RawSample{0, 0, 256});
        bus.set_self_test_delta(adxl345::RawSample{100, -100, 200});

        adxl345::SelfTestOptions options;
        options.samples = 4;
        options.discard = 0;
        options.settle_ms = 0;
        options.supply_volts = 2.5;
        const adxl345::SelfTestResult result = device.run_self_test(options);
        CHECK_EQ(result.x_lsb, 100);
        CHECK_EQ(result.y_lsb, -100);
        CHECK_EQ(result.z_lsb, 200);
        CHECK_NEAR(result.x_g, 100.0 / 256.0, 1e-12);
        CHECK(result.within_window);
        CHECK(result.meets_minimum);
        CHECK_EQ(result.limits.min_x, 50);
        CHECK_EQ(result.limits.max_y, -50);

        CHECK_EQ(bus.reg(adxl345::reg::data_format), 0x00);
        CHECK_EQ(bus.reg(adxl345::reg::bw_rate), 0x0A);
        CHECK_EQ(bus.reg(adxl345::reg::power_ctl), 0x00);

        bool saw_format = false;
        bool saw_self_test = false;
        for (const adxl345::testutil::Xfer& xfer : bus.log()) {
            if (xfer.kind == adxl345::testutil::XferKind::write && xfer.reg == adxl345::reg::data_format &&
                !xfer.data.empty()) {
                if (xfer.data[0] == 0x0B) {
                    saw_format = true;
                }
                if (xfer.data[0] == 0x8B) {
                    saw_self_test = true;
                }
            }
        }
        CHECK(saw_format);
        CHECK(saw_self_test);
    });

    suite.test("a too-small self-test delta fails the minimum", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        device.start();
        bus.set_sample({});
        bus.set_self_test_delta(adxl345::RawSample{10, -10, 10});
        adxl345::SelfTestOptions options;
        options.samples = 1;
        options.discard = 0;
        options.settle_ms = 0;
        options.supply_volts = 2.5;
        const adxl345::SelfTestResult result = device.run_self_test(options);
        CHECK(!result.meets_minimum);
        CHECK(!result.within_window);
    });

    suite.test("bad self-test voltage does not change the part", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        bus.clear_log();
        bool threw = false;
        try {
            adxl345::SelfTestOptions options;
            options.supply_volts = 5.0;
            (void)device.run_self_test(options);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
        CHECK_EQ(bus.log().size(), 0u);
    });

    suite.test("interrupt publish drop trace", [] {
        adxl345_u32 sequence = 0;
        adxl345_u32 read_seq = 0;
        adxl345_u32 dropped = 0;
        const adxl345_u32 expected_drop[] = {0u, 1u, 2u, 3u};
        for (adxl345_u32 expect_dropped : expected_drop) {
            const adxl345_u32 before = sequence;
            const adxl345_u32 published =
                adxl345_custom_note_publish(&sequence, read_seq, &dropped);
            CHECK_EQ(published, before + 1u);
            CHECK_EQ(published, sequence);
            CHECK_EQ(dropped, expect_dropped);
        }
        read_seq = sequence;
        for (int i = 0; i < 5; ++i) {
            const adxl345_u32 before_dropped = dropped;
            const adxl345_u32 published =
                adxl345_custom_note_publish(&sequence, read_seq, &dropped);
            read_seq = published;
            CHECK_EQ(dropped, before_dropped);
        }
        const adxl345_u32 caught_up = dropped;
        (void)adxl345_custom_note_publish(&sequence, read_seq, &dropped);
        (void)adxl345_custom_note_publish(&sequence, read_seq, &dropped);
        CHECK_EQ(dropped, caught_up + 1u);
        CHECK_EQ(sequence, 11u);
    });
}
