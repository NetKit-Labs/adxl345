#include "harness.hpp"
#include "mock_bus.hpp"

#include "adxl345/driver.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

void run_unit_tests() {
    test::Suite suite{"unit"};

    suite.test("scale factors from Table 1", [] {
        CHECK_EQ(adxl345::nominal_lsb_per_g(adxl345::Range::g2, false), 256);
        CHECK_EQ(adxl345::nominal_lsb_per_g(adxl345::Range::g4, false), 128);
        CHECK_EQ(adxl345::nominal_lsb_per_g(adxl345::Range::g8, false), 64);
        CHECK_EQ(adxl345::nominal_lsb_per_g(adxl345::Range::g16, false), 32);
        CHECK_EQ(adxl345::nominal_lsb_per_g(adxl345::Range::g2, true), 256);
        CHECK_EQ(adxl345::nominal_lsb_per_g(adxl345::Range::g4, true), 256);
        CHECK_EQ(adxl345::nominal_lsb_per_g(adxl345::Range::g8, true), 256);
        CHECK_EQ(adxl345::nominal_lsb_per_g(adxl345::Range::g16, true), 256);
    });

    suite.test("resolution grows only in full-resolution mode", [] {
        CHECK_EQ(adxl345::resolution_bits(adxl345::Range::g2, false), 10);
        CHECK_EQ(adxl345::resolution_bits(adxl345::Range::g16, false), 10);
        CHECK_EQ(adxl345::resolution_bits(adxl345::Range::g4, true), 11);
        CHECK_EQ(adxl345::resolution_bits(adxl345::Range::g8, true), 12);
        CHECK_EQ(adxl345::resolution_bits(adxl345::Range::g16, true), 13);
    });

    suite.test("output data rate and bandwidth tables", [] {
        const adxl345::DataRate rates[] = {
            adxl345::DataRate::hz0_10, adxl345::DataRate::hz0_20, adxl345::DataRate::hz0_39,
            adxl345::DataRate::hz0_78, adxl345::DataRate::hz1_56, adxl345::DataRate::hz3_13,
            adxl345::DataRate::hz6_25, adxl345::DataRate::hz12_5, adxl345::DataRate::hz25,
            adxl345::DataRate::hz50,   adxl345::DataRate::hz100,  adxl345::DataRate::hz200,
            adxl345::DataRate::hz400,  adxl345::DataRate::hz800,  adxl345::DataRate::hz1600,
            adxl345::DataRate::hz3200};
        const double hz[] = {0.10, 0.20, 0.39, 0.78, 1.56, 3.13, 6.25, 12.5,
                             25,   50,   100,  200,  400,  800,  1600, 3200};
        const double bw[] = {0.05, 0.10, 0.20, 0.39, 0.78, 1.56, 3.13, 6.25,
                             12.5, 25,   50,   100,  200,  400,  800,  1600};
        for (int i = 0; i < 16; ++i) {
            CHECK_EQ(static_cast<int>(rates[i]), i);
            CHECK_NEAR(adxl345::data_rate_hz(rates[i]), hz[i], 1e-9);
            CHECK_NEAR(adxl345::bandwidth_hz(rates[i]), bw[i], 1e-9);
        }
        CHECK_NEAR(adxl345::max_recommended_odr_hz(100000.0), 200.0, 1e-9);
        CHECK_NEAR(adxl345::max_recommended_odr_hz(400000.0), 800.0, 1e-9);
    });

    suite.test("data format and power round-trip", [] {
        for (int range = 0; range < 4; ++range) {
            for (int mask = 0; mask < 32; ++mask) {
                adxl345::DataFormat format;
                format.self_test = (mask & 1) != 0;
                format.spi_3wire = (mask & 2) != 0;
                format.interrupt_active_low = (mask & 4) != 0;
                format.full_resolution = (mask & 8) != 0;
                format.left_justified = (mask & 16) != 0;
                format.range = static_cast<adxl345::Range>(range);
                CHECK(adxl345::decode_data_format(adxl345::encode(format)) == format);
            }
        }
        adxl345::PowerControl power{.link = true, .auto_sleep = true, .measure = true, .sleep = true,
                                    .wakeup = adxl345::WakeupFrequency::hz1};
        CHECK(adxl345::decode_power(adxl345::encode(power)) == power);
        CHECK_EQ(adxl345::encode(power), 0x3F);
    });

    suite.test("little-endian sample decode includes negatives", [] {
        const std::uint8_t bytes[] = {0x00, 0x01, 0xFF, 0xFF, 0xFE, 0xFF};
        const adxl345::RawSample sample = adxl345::decode_le(std::span<const std::uint8_t, 6>{bytes});
        CHECK_EQ(sample.x, 256);
        CHECK_EQ(sample.y, -1);
        CHECK_EQ(sample.z, -2);
    });

    suite.test("left-justified samples shift back to counts", [] {
        adxl345::DataFormat format{.full_resolution = false, .left_justified = true, .range = adxl345::Range::g2};
        const adxl345::RawSample normalized = adxl345::normalize(adxl345::RawSample{0x0040, static_cast<std::int16_t>(0xFFC0), 0x4000}, format);
        CHECK_EQ(normalized.x, 1);
        CHECK_EQ(normalized.y, -1);
        CHECK_EQ(normalized.z, 256);

        adxl345::DataFormat wide{.full_resolution = true, .left_justified = true, .range = adxl345::Range::g16};
        const adxl345::RawSample wide_counts = adxl345::normalize(adxl345::RawSample{1 << 3, 0, 0}, wide);
        CHECK_EQ(wide_counts.x, 1);
    });

    suite.test("counts convert at the nominal scale", [] {
        adxl345::DataFormat full{.full_resolution = true, .range = adxl345::Range::g16};
        const adxl345::Acceleration one_g = adxl345::counts_to_g(adxl345::RawSample{0, 0, 256}, full);
        CHECK_NEAR(one_g.z_g, 1.0, 1e-12);
        CHECK_NEAR(one_g.magnitude_g(), 1.0, 1e-12);

        adxl345::DataFormat ten_bit{.full_resolution = false, .range = adxl345::Range::g4};
        const adxl345::Acceleration minus_one = adxl345::counts_to_g(adxl345::RawSample{0, -128, 0}, ten_bit);
        CHECK_NEAR(minus_one.y_g, -1.0, 1e-12);

        adxl345::DataFormat sixteen{.full_resolution = false, .range = adxl345::Range::g16};
        const adxl345::Acceleration coarse = adxl345::counts_to_g(adxl345::RawSample{32, 0, 0}, sixteen);
        CHECK_NEAR(coarse.x_g, 1.0, 1e-12);
    });

    suite.test("datasheet offset example rounds half away from zero", [] {
        const adxl345::RawSample measured{10, -13, 9};
        const adxl345::AxisOffsets offsets = adxl345::compute_offsets(std::span(&measured, 1), {}, 256);
        CHECK_EQ(offsets.x, -3);
        CHECK_EQ(offsets.y, 3);
        CHECK_EQ(offsets.z, -2);
        CHECK_EQ(static_cast<std::uint8_t>(offsets.x), 0xFD);
        CHECK_EQ(static_cast<std::uint8_t>(offsets.y), 0x03);
        CHECK_EQ(static_cast<std::uint8_t>(offsets.z), 0xFE);
    });

    suite.test("offset ratio follows the data LSB", [] {
        const adxl345::RawSample error{1, -1, 2};
        const adxl345::AxisOffsets wide = adxl345::compute_offsets(std::span(&error, 1), {}, 32);
        CHECK_EQ(wide.x, -2);
        CHECK_EQ(wide.y, 2);
        CHECK_EQ(wide.z, -4);

        const adxl345::RawSample half{2, 0, 0};
        const adxl345::AxisOffsets tie = adxl345::compute_offsets(std::span(&half, 1), {}, 256);
        CHECK_EQ(tie.x, -1);
    });

    suite.test("offset calibration subtracts the ideal gravity reading", [] {
        const adxl345::RawSample z_up{10, -13, 265};
        const adxl345::AxisOffsets offsets =
            adxl345::compute_offsets(std::span(&z_up, 1), adxl345::RawSample{0, 0, 256}, 256);
        CHECK_EQ(offsets.x, -3);
        CHECK_EQ(offsets.y, 3);
        CHECK_EQ(offsets.z, -2);
    });

    suite.test("offset helpers reject empty input and clamp", [] {
        bool threw = false;
        try {
            (void)adxl345::compute_offsets({}, {}, 256);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);

        const adxl345::RawSample huge{32000, 0, 0};
        const adxl345::AxisOffsets clamped = adxl345::compute_offsets(std::span(&huge, 1), {}, 256);
        CHECK_EQ(clamped.x, -128);
    });

    suite.test("threshold and timing quantizers", [] {
        CHECK_EQ(adxl345::quantize_threshold_g(0.0), 0);
        CHECK_EQ(adxl345::quantize_threshold_g(0.0625), 1);
        CHECK_EQ(adxl345::quantize_threshold_g(3.0), 48);
        CHECK_EQ(adxl345::quantize_threshold_g(16.0), 255);
        CHECK_EQ(adxl345::quantize_tap_duration_s(0.010), 16);
        CHECK_EQ(adxl345::quantize_tap_window_s(0.080), 64);
        CHECK_EQ(adxl345::quantize_free_fall_time_s(0.160), 32);
        CHECK_EQ(adxl345::quantize_inactivity_time_s(5.0), 5);
        bool threw = false;
        try {
            (void)adxl345::quantize_threshold_g(-0.1);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    });

    suite.test("self-test limits match Tables 15 through 18 at 2.5 V", [] {
        const adxl345::DataFormat full{.full_resolution = true, .range = adxl345::Range::g16};
        const adxl345::SelfTestLimits table15 = adxl345::self_test_limits(full, 2.5);
        CHECK_EQ(table15.min_x, 50);
        CHECK_EQ(table15.max_x, 540);
        CHECK_EQ(table15.min_y, -540);
        CHECK_EQ(table15.max_y, -50);
        CHECK_EQ(table15.min_z, 75);
        CHECK_EQ(table15.max_z, 875);

        const adxl345::DataFormat g4{.full_resolution = false, .range = adxl345::Range::g4};
        const adxl345::SelfTestLimits table16 = adxl345::self_test_limits(g4, 2.5);
        CHECK_EQ(table16.min_x, 25);
        CHECK_EQ(table16.max_y, -25);
        CHECK_EQ(table16.max_z, 438);

        const adxl345::DataFormat g8{.full_resolution = false, .range = adxl345::Range::g8};
        CHECK_EQ(adxl345::self_test_limits(g8, 2.5).min_x, 12);

        const adxl345::DataFormat g16{.full_resolution = false, .range = adxl345::Range::g16};
        const adxl345::SelfTestLimits table18 = adxl345::self_test_limits(g16, 2.5);
        CHECK_EQ(table18.min_x, 6);
        CHECK_EQ(table18.max_x, 67);
        CHECK_EQ(table18.min_z, 10);
        CHECK_EQ(table18.max_z, 110);
    });

    suite.test("self-test limits scale with supply voltage", [] {
        const adxl345::DataFormat full{.full_resolution = true, .range = adxl345::Range::g2};
        const adxl345::SelfTestLimits at_3v3 = adxl345::self_test_limits(full, 3.3);
        CHECK_EQ(at_3v3.min_x, 89);
        CHECK_EQ(at_3v3.max_x, 956);
        CHECK_EQ(at_3v3.min_y, -956);
        CHECK_EQ(at_3v3.max_y, -89);
        CHECK_EQ(at_3v3.min_z, 110);
        CHECK_EQ(at_3v3.max_z, 1286);

        const adxl345::SelfTestLimits mid = adxl345::self_test_limits(full, 2.25);
        CHECK_EQ(mid.min_x, static_cast<int>(std::lround(50.0 * 0.82)));
        CHECK_EQ(mid.min_z, static_cast<int>(std::lround(75.0 * 0.90)));

        bool low = false;
        bool high = false;
        try {
            (void)adxl345::self_test_limits(full, 1.9);
        } catch (const std::invalid_argument&) {
            low = true;
        }
        try {
            (void)adxl345::self_test_limits(full, 3.7);
        } catch (const std::invalid_argument&) {
            high = true;
        }
        CHECK(low);
        CHECK(high);
    });

    suite.test("fifo and interrupt field packing", [] {
        const adxl345::FifoControl fifo{.mode = adxl345::FifoMode::fifo, .trigger_int2 = false, .samples = 31};
        CHECK_EQ(adxl345::encode(fifo), 0x5F);
        CHECK(adxl345::decode_fifo(0xB0) ==
              (adxl345::FifoControl{.mode = adxl345::FifoMode::stream, .trigger_int2 = true, .samples = 16}));
        const adxl345::FifoStatus status = adxl345::decode_fifo_status(0xA0);
        CHECK(status.triggered);
        CHECK_EQ(status.entries, 32);
        CHECK_EQ(adxl345::reg::irq::data_ready, 0x80);
        CHECK_EQ(adxl345::reg::irq::overrun, 0x01);
        CHECK_EQ(static_cast<int>(adxl345::reg::irq::data_ready | adxl345::reg::irq::overrun), 0x81);
    });

    suite.test("activity and tap status bits", [] {
        const adxl345::ActTapStatus status = adxl345::decode_act_tap_status(0x6A);
        CHECK(status.activity_x);
        CHECK(status.activity_y);
        CHECK(!status.activity_z);
        CHECK(status.asleep);
        CHECK(!status.tap_x);
        CHECK(status.tap_y);
        CHECK(!status.tap_z);
    });

    suite.test("driver rejects a bad device id before configuring", [] {
        adxl345::testutil::MockBus bus;
        bus.set_device_id(0x00);
        adxl345::Adxl345 device(bus);
        bool threw = false;
        try {
            device.verify();
        } catch (const adxl345::Error& error) {
            threw = error.code() == adxl345::Error::Code::bad_device_id;
        }
        CHECK(threw);
        CHECK_EQ(bus.log().size(), 1u);
        CHECK(bus.log()[0].kind == adxl345::testutil::XferKind::read);
        CHECK_EQ(bus.log()[0].reg, adxl345::reg::devid);
    });

    suite.test("standby reads are zero and measurement returns the sample", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        bus.set_sample(adxl345::RawSample{1, 2, 3});
        const adxl345::RawSample asleep = device.read_raw();
        CHECK_EQ(asleep.x, 0);
        CHECK_EQ(asleep.y, 0);
        CHECK_EQ(asleep.z, 0);
        device.start();
        bus.set_sample(adxl345::RawSample{4, -5, 256});
        const adxl345::Acceleration g = device.read_g();
        CHECK_NEAR(g.x_g, 4.0 / 256.0, 1e-12);
        CHECK_NEAR(g.y_g, -5.0 / 256.0, 1e-12);
        CHECK_NEAR(g.z_g, 1.0, 1e-12);
    });

    suite.test("bus failures propagate", [] {
        adxl345::testutil::MockBus bus;
        adxl345::Adxl345 device(bus);
        bus.fail_next(EIO);
        bool threw = false;
        try {
            (void)device.device_id();
        } catch (const std::system_error&) {
            threw = true;
        }
        CHECK(threw);
    });
}
