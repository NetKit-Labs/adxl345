#include "adxl345/adxl345.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string bus = "/dev/i2c-1";
    int address = -1;
    int count = 20;
    adxl345::DataRate rate = adxl345::DataRate::hz100;
    int range_g = 16;
    bool full_resolution = true;
    bool low_power = false;
    double supply_volts = 3.3;
    bool z_down = false;
    std::string command;
};

void usage() {
    std::println(stderr, "usage: adxl345 <scan|id|read|dump|self-test|calibrate> [options]");
    std::println(stderr, "  --bus PATH       I2C device node (default /dev/i2c-1)");
    std::println(stderr, "  --addr 0x53      7-bit address (default: probe 0x53 then 0x1D)");
    std::println(stderr, "  --count N        samples to print (default 20)");
    std::println(stderr, "  --rate HZ        0.10, 0.20, 0.39, 0.78, 1.56, 3.13, 6.25, 12.5,");
    std::println(stderr, "                   25, 50, 100, 200, 400, 800 (default 100)");
    std::println(stderr, "  --range 2|4|8|16 g range (default 16)");
    std::println(stderr, "  --full-res       3.9 mg/LSB at every range (default)");
    std::println(stderr, "  --10-bit         fixed 10-bit mode");
    std::println(stderr, "  --low-power      set BW_RATE low-power bit");
    std::println(stderr, "  --vs VOLTS       sensor supply for self-test limits (default 3.3)");
    std::println(stderr, "  --z-down         calibrate with -1 g on Z");
}

[[nodiscard]] bool parse_rate(const std::string& text, adxl345::DataRate& rate) {
    struct Item {
        const char* name;
        adxl345::DataRate rate;
    };
    const Item items[] = {
        {"0.10", adxl345::DataRate::hz0_10}, {"0.1", adxl345::DataRate::hz0_10},
        {"0.20", adxl345::DataRate::hz0_20}, {"0.2", adxl345::DataRate::hz0_20},
        {"0.39", adxl345::DataRate::hz0_39}, {"0.78", adxl345::DataRate::hz0_78},
        {"1.56", adxl345::DataRate::hz1_56}, {"3.13", adxl345::DataRate::hz3_13},
        {"6.25", adxl345::DataRate::hz6_25}, {"12.5", adxl345::DataRate::hz12_5},
        {"25", adxl345::DataRate::hz25},     {"50", adxl345::DataRate::hz50},
        {"100", adxl345::DataRate::hz100},   {"200", adxl345::DataRate::hz200},
        {"400", adxl345::DataRate::hz400},   {"800", adxl345::DataRate::hz800},
    };
    for (const Item& item : items) {
        if (text == item.name) {
            rate = item.rate;
            return true;
        }
    }
    return false;
}

[[nodiscard]] int parse_address(const std::string& text) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 0);
    if (end == text.c_str() || *end != '\0' || value < 0 || value > 0x7F) {
        throw std::invalid_argument("address must be a 7-bit number, for example 0x53");
    }
    return static_cast<int>(value);
}

[[nodiscard]] Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* name) {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string("missing value for ") + name);
            }
            return std::string(argv[++i]);
        };
        if (arg == "--help" || arg == "-h") {
            usage();
            std::exit(0);
        } else if (arg == "--bus") {
            options.bus = need("--bus");
        } else if (arg == "--addr") {
            options.address = parse_address(need("--addr"));
        } else if (arg == "--count") {
            options.count = std::stoi(need("--count"));
        } else if (arg == "--rate") {
            const std::string text = need("--rate");
            if (!parse_rate(text, options.rate)) {
                throw std::invalid_argument("unsupported data rate " + text);
            }
        } else if (arg == "--range") {
            options.range_g = std::stoi(need("--range"));
        } else if (arg == "--full-res") {
            options.full_resolution = true;
        } else if (arg == "--10-bit") {
            options.full_resolution = false;
        } else if (arg == "--low-power") {
            options.low_power = true;
        } else if (arg == "--vs") {
            options.supply_volts = std::stod(need("--vs"));
        } else if (arg == "--z-down") {
            options.z_down = true;
        } else if (!arg.empty() && arg[0] != '-') {
            if (!options.command.empty()) {
                throw std::invalid_argument("only one command is accepted");
            }
            options.command = arg;
        } else {
            throw std::invalid_argument("unknown option " + arg);
        }
    }
    if (options.command.empty()) {
        throw std::invalid_argument("missing command");
    }
    return options;
}

[[nodiscard]] adxl345::Range range_from_g(int range_g) {
    switch (range_g) {
        case 2:
            return adxl345::Range::g2;
        case 4:
            return adxl345::Range::g4;
        case 8:
            return adxl345::Range::g8;
        case 16:
            return adxl345::Range::g16;
        default:
            throw std::invalid_argument("range must be 2, 4, 8, or 16");
    }
}

[[nodiscard]] std::uint8_t resolve_address(const Options& options) {
    if (options.address >= 0) {
        return static_cast<std::uint8_t>(options.address);
    }
    const std::optional<std::uint8_t> found = adxl345::probe_adxl345(options.bus);
    if (!found) {
        throw std::runtime_error(
            "no ADXL345 responded on " + options.bus +
            ". Enable I2C (sudo raspi-config nonint do_i2c 0) and check SDA, SCL, power, and ground.");
    }
    return *found;
}

void configure_measurement(adxl345::Adxl345& device, const Options& options) {
    device.set_data_format(adxl345::DataFormat{
        .full_resolution = options.full_resolution,
        .range = range_from_g(options.range_g),
    });
    device.set_data_rate(options.rate, options.low_power);
    device.start();
}

void print_sample(int index, adxl345::RawSample raw, adxl345::Acceleration g) {
    std::println("{:4}  raw {:+6} {:+6} {:+6}   g {:+8.4f} {:+8.4f} {:+8.4f}   |a|={:.4f}", index, raw.x, raw.y,
                 raw.z, g.x_g, g.y_g, g.z_g, g.magnitude_g());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse(argc, argv);
        if (options.command == "scan") {
            const std::optional<std::uint8_t> found = adxl345::probe_adxl345(options.bus);
            if (!found) {
                std::println("no ADXL345 on {}", options.bus);
                return 1;
            }
            std::println("ADXL345 at 0x{:02X} on {}", *found, options.bus);
            return 0;
        }

        adxl345::LinuxI2cBus bus(options.bus, resolve_address(options));
        adxl345::Adxl345 device(bus);
        device.verify();

        if (options.command == "id") {
            const adxl345::PowerControl power = device.power();
            const adxl345::DataFormat format = device.data_format();
            std::println("DEVID 0xE5");
            std::println("address 0x{:02X}", bus.address());
            std::println("rate {}", adxl345::to_string(device.data_rate()));
            std::println("low_power {}", device.low_power() ? "yes" : "no");
            std::println("range {}{}", adxl345::to_string(format.range), format.full_resolution ? " full-res" : " 10-bit");
            std::println("measuring {}", power.measure ? "yes" : "no");
            return 0;
        }
        if (options.command == "dump") {
            std::println("DEVID 0x{:02X}", device.read_register(adxl345::reg::devid));
            for (std::uint8_t reg = adxl345::reg::thresh_tap; reg <= adxl345::reg::fifo_status; ++reg) {
                std::println("0x{:02X} 0x{:02X}", reg, device.read_register(reg));
            }
            return 0;
        }
        if (options.command == "read") {
            if (options.count <= 0) {
                throw std::invalid_argument("count must be positive");
            }
            configure_measurement(device, options);
            const double odr = adxl345::data_rate_hz(device.data_rate());
            if (odr > adxl345::max_recommended_odr_hz(100000.0)) {
                std::println(stderr, "note: {} exceeds the 200 Hz recommendation at the Pi's default 100 kHz I2C clock",
                             adxl345::to_string(device.data_rate()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const auto timeout = std::chrono::milliseconds(static_cast<int>(1000.0 / odr) + 50);
            for (int i = 0; i < options.count; ++i) {
                const adxl345::RawSample raw = device.read_raw_when_ready(timeout);
                const adxl345::DataFormat format = device.data_format();
                print_sample(i, raw, adxl345::counts_to_g(adxl345::normalize(raw, format), format));
            }
            return 0;
        }
        if (options.command == "self-test") {
            adxl345::SelfTestOptions self_test;
            self_test.supply_volts = options.supply_volts;
            const adxl345::SelfTestResult result = device.run_self_test(self_test);
            std::println("off  {:+6} {:+6} {:+6}", result.off.x, result.off.y, result.off.z);
            std::println("on   {:+6} {:+6} {:+6}", result.on.x, result.on.y, result.on.z);
            std::println("delta LSB {:+6} {:+6} {:+6}", result.x_lsb, result.y_lsb, result.z_lsb);
            std::println("delta g   {:+8.3f} {:+8.3f} {:+8.3f}", result.x_g, result.y_g, result.z_g);
            std::println("limits X [{}, {}]  Y [{}, {}]  Z [{}, {}] at {:.2f} V", result.limits.min_x,
                         result.limits.max_x, result.limits.min_y, result.limits.max_y, result.limits.min_z,
                         result.limits.max_z, options.supply_volts);
            std::println("minimum magnitude {}", result.meets_minimum ? "pass" : "fail");
            std::println("full window {}", result.within_window ? "pass" : "outside");
            return result.meets_minimum ? 0 : 1;
        }
        if (options.command == "calibrate") {
            device.set_data_format(adxl345::DataFormat{.full_resolution = true, .range = adxl345::Range::g16});
            device.set_data_rate(adxl345::DataRate::hz100, false);
            device.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const adxl345::RawSample ideal{0, 0, static_cast<std::int16_t>(options.z_down ? -256 : 256)};
            const adxl345::AxisOffsets offsets = device.calibrate(32, ideal);
            std::println("offsets x={} y={} z={} (15.6 mg/LSB, lost at power-off)", static_cast<int>(offsets.x),
                         static_cast<int>(offsets.y), static_cast<int>(offsets.z));
            return 0;
        }
        throw std::invalid_argument("unknown command " + options.command);
    } catch (const std::exception& ex) {
        std::println(stderr, "{}", ex.what());
        return 1;
    }
}
