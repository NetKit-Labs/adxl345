#include "adxl345/adxl345.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const std::string& name, const std::string& detail) {
    if (ok) {
        std::println("PASS  {}", name);
        return;
    }
    ++failures;
    std::println("FAIL  {}  {}", name, detail);
}

[[nodiscard]] std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || value[0] == '\0' ? fallback : std::string(value);
}

}  // namespace

int main() {
    const std::string bus_path = env_or("ADXL345_BUS", "/dev/i2c-1");
    const std::string vs_text = env_or("ADXL345_VS", "3.3");
    double supply_volts = 3.3;
    try {
        supply_volts = std::stod(vs_text);
    } catch (const std::exception&) {
        std::println(stderr, "ADXL345_VS must be a voltage from 2.0 to 3.6");
        return 2;
    }

    if (::access(bus_path.c_str(), F_OK) != 0) {
        std::println(stderr, "I2C node {} does not exist. On the Pi: sudo raspi-config nonint do_i2c 0", bus_path);
        return 2;
    }

    std::optional<std::uint8_t> address;
    const std::string addr_text = env_or("ADXL345_ADDR", "");
    try {
        if (!addr_text.empty()) {
            address = static_cast<std::uint8_t>(std::stoul(addr_text, nullptr, 0));
        } else {
            address = adxl345::probe_adxl345(bus_path);
        }
    } catch (const std::exception& ex) {
        std::println(stderr, "{}", ex.what());
        std::println(stderr, "I2C node {} is not usable. On the Pi: sudo raspi-config nonint do_i2c 0", bus_path);
        return 2;
    }
    if (!address) {
        std::println(stderr, "no ADXL345 (DEVID 0xE5) on {} at 0x53 or 0x1D", bus_path);
        std::println(stderr, "Check power, ground, SDA on GPIO2, SCL on GPIO3, and that CS is high.");
        return 2;
    }

    try {
        adxl345::LinuxI2cBus bus(bus_path, *address);
        adxl345::Adxl345 device(bus);
        std::println("bus {} address 0x{:02X} supply {:.2f} V", bus_path, *address, supply_volts);

        bool id_ok = false;
        try {
            device.verify();
            id_ok = true;
        } catch (const std::exception& ex) {
            expect(false, "device id", ex.what());
        }
        expect(id_ok, "device id", "DEVID was not 0xE5");

        const std::uint8_t saved_tap = device.read_register(adxl345::reg::thresh_tap);
        device.write_register(adxl345::reg::thresh_tap, 0x55);
        const bool readback = device.read_register(adxl345::reg::thresh_tap) == 0x55;
        device.write_register(adxl345::reg::thresh_tap, saved_tap);
        const bool restored = device.read_register(adxl345::reg::thresh_tap) == saved_tap;
        expect(readback && restored, "register readback", "THRESH_TAP did not stick");

        const adxl345::AxisOffsets saved_offsets = device.offsets();
        device.set_offsets({});
        device.set_data_format(adxl345::DataFormat{.full_resolution = true, .range = adxl345::Range::g16});
        device.set_data_rate(adxl345::DataRate::hz100, false);
        device.start();
        expect(device.measuring(), "measure bit", "POWER_CTL.measure did not set");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        constexpr int kSamples = 32;
        std::vector<adxl345::Acceleration> samples;
        samples.reserve(kSamples);
        bool reads_ok = true;
        std::string read_error;
        try {
            for (int i = 0; i < kSamples; ++i) {
                const adxl345::RawSample raw = device.read_raw_when_ready(std::chrono::milliseconds(80));
                const adxl345::DataFormat format = device.data_format();
                const adxl345::Acceleration g = adxl345::counts_to_g(adxl345::normalize(raw, format), format);
                samples.push_back(g);
                if (i < 4) {
                    std::println("  sample {}  {:+7.3f} {:+7.3f} {:+7.3f} g  |a|={:.3f}", i, g.x_g, g.y_g, g.z_g,
                                 g.magnitude_g());
                }
            }
        } catch (const std::exception& ex) {
            reads_ok = false;
            read_error = ex.what();
        }
        expect(reads_ok && samples.size() == static_cast<std::size_t>(kSamples), "sample burst",
               read_error.empty() ? "short read" : read_error);

        if (!samples.empty()) {
            double sum = 0;
            for (const adxl345::Acceleration& sample : samples) {
                sum += sample.magnitude_g();
            }
            const double mean = sum / static_cast<double>(samples.size());
            double worst = 0;
            for (const adxl345::Acceleration& sample : samples) {
                worst = std::max(worst, std::abs(sample.magnitude_g() - mean));
            }
            std::println("  mean |a|={:.3f} g  max deviation {:.3f} g", mean, worst);
            expect(mean >= 0.45 && mean <= 1.75, "gravity magnitude",
                   "expected about 1 g while the board is sitting still");
            expect(worst <= 1.0, "stationary samples", "successive samples disagreed by more than 1 g");
        }

        adxl345::SelfTestOptions self_test;
        self_test.supply_volts = supply_volts;
        bool self_test_ran = false;
        adxl345::SelfTestResult result;
        try {
            result = device.run_self_test(self_test);
            self_test_ran = true;
        } catch (const std::exception& ex) {
            expect(false, "self-test", ex.what());
        }
        if (self_test_ran) {
            std::println("  self-test delta {:+.3f} {:+.3f} {:+.3f} g", result.x_g, result.y_g, result.z_g);
            std::println("  limits X [{}, {}] Y [{}, {}] Z [{}, {}] LSB", result.limits.min_x, result.limits.max_x,
                         result.limits.min_y, result.limits.max_y, result.limits.min_z, result.limits.max_z);
            expect(result.meets_minimum, "self-test minimum",
                   "electrostatic shift was below the datasheet minimum; try ADXL345_VS=3.0 if VIN is 3.3 V through the regulator");
            if (result.meets_minimum && !result.within_window) {
                std::println("NOTE  self-test exceeded the maximum shift; the datasheet does not treat that alone as a failure");
            }
        }

        device.standby();
        expect(!device.measuring(), "standby", "measure bit stayed set");
        device.set_offsets(saved_offsets);
    } catch (const std::exception& ex) {
        std::println(stderr, "{}", ex.what());
        return 1;
    }

    if (failures == 0) {
        std::println("all hardware checks passed");
    } else {
        std::println("{} hardware checks failed", failures);
    }
    return failures == 0 ? 0 : 1;
}
