// Hardware check for data-ready interrupts on GPIO17 (Pi pin 11).
// The module must already be loaded. This does not insmod or rmmod.

#include "adxl345/custom_mod_abi.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr int kSamples = 64;
constexpr double kLsbPerG = 256.0;
constexpr const char *kSysfs = "/sys/bus/i2c/devices/1-0053";

[[nodiscard]] std::string trim(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.pop_back();
  }
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  return text.substr(begin);
}

[[nodiscard]] std::string read_text(const fs::path &path) {
  std::ifstream input(path);
  if (!input) {
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return trim(buffer.str());
}

[[nodiscard]] unsigned long read_counter(const char *name) {
  try {
    return std::stoul(read_text(fs::path(kSysfs) / name));
  } catch (const std::exception &) {
    return 0;
  }
}

[[nodiscard]] bool expect(bool ok, std::string_view name, std::string_view detail) {
  std::println("{}  {}", ok ? "PASS" : "FAIL", name);
  if (!ok && !detail.empty()) {
    std::println("      {}", detail);
  }
  return ok;
}

struct Fd {
  int fd = -1;
  ~Fd() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  Fd() = default;
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
};

}  // namespace

int main() {
  const std::string mode = read_text(fs::path(kSysfs) / "mode");
  const std::string line = read_text(fs::path(kSysfs) / "int_line");
  const std::string trigger = read_text(fs::path(kSysfs) / "trigger");
  const std::string gpio = read_text(fs::path(kSysfs) / "irq_gpio");
  int failures = 0;

  std::println("ADXL345 data-ready interrupt check");
  std::println("mode {}  int_line {}  trigger {}  gpio {}",
               mode.empty() ? "(missing)" : mode,
               line.empty() ? "(missing)" : line,
               trigger.empty() ? "(missing)" : trigger,
               gpio.empty() ? "(missing)" : gpio);

  if (!expect(mode == "irq", "interrupt mode", "sysfs mode is not irq; the INT wire did not deliver data-ready")) {
    return 2;
  }
  if (!expect(gpio == "17", "GPIO17", "irq_gpio sysfs is not 17")) {
    ++failures;
  }
  if (!expect(line == "1" || line == "2", "INT line selected", "int_line was not 1 or 2")) {
    ++failures;
  }

  Fd device;
  device.fd = ::open("/dev/adxl345_custom", O_RDONLY);
  if (device.fd < 0) {
    std::println(std::cerr, "open /dev/adxl345_custom: {}", std::strerror(errno));
    return 2;
  }

  const unsigned long errors_before = read_counter("i2c_errors");
  const unsigned long dropped_before = read_counter("dropped");
  std::vector<adxl345_custom_sample> samples;
  samples.reserve(static_cast<std::size_t>(kSamples));
  for (int i = 0; i < kSamples; ++i) {
    adxl345_custom_sample sample{};
    const ssize_t n = ::read(device.fd, &sample, sizeof(sample));
    if (n != static_cast<ssize_t>(sizeof(sample))) {
      std::println("FAIL  blocking read {} returned {} ({})", i, n, std::strerror(errno));
      return 1;
    }
    samples.push_back(sample);
  }
  const unsigned long dropped_after_reads = read_counter("dropped");

  const int nonblock = ::fcntl(device.fd, F_GETFL, 0);
  if (nonblock >= 0) {
    ::fcntl(device.fd, F_SETFL, nonblock | O_NONBLOCK);
  }
  adxl345_custom_sample extra{};
  int queued = 0;
  while (queued < 4) {
    const ssize_t n = ::read(device.fd, &extra, sizeof(extra));
    if (n == static_cast<ssize_t>(sizeof(extra))) {
      ++queued;
      continue;
    }
    if (!expect(n < 0 && errno == EAGAIN, "nonblocking read",
                "expected EAGAIN once the latest sample was consumed")) {
      ++failures;
    }
    break;
  }
  if (nonblock >= 0) {
    ::fcntl(device.fd, F_SETFL, nonblock);
  }
  if (!expect(queued <= 1, "single latched sample", "more than one unread sample was queued")) {
    ++failures;
  }

  std::vector<double> intervals_ms;
  int steps_of_one = 0;
  int steps = 0;
  double sx = 0;
  double sy = 0;
  double sz = 0;
  bool ordered = true;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    sx += samples[i].x;
    sy += samples[i].y;
    sz += samples[i].z;
    if (i == 0) {
      continue;
    }
    ++steps;
    if (samples[i].sequence != samples[i - 1].sequence + 1) {
      if (samples[i].sequence <= samples[i - 1].sequence) {
        ordered = false;
      }
    } else {
      ++steps_of_one;
    }
    if (samples[i].timestamp_ns <= samples[i - 1].timestamp_ns) {
      ordered = false;
    }
    intervals_ms.push_back(static_cast<double>(samples[i].timestamp_ns - samples[i - 1].timestamp_ns) /
                           1.0e6);
  }
  const double n = static_cast<double>(samples.size());
  const double gx = (sx / n) / kLsbPerG;
  const double gy = (sy / n) / kLsbPerG;
  const double gz = (sz / n) / kLsbPerG;
  const double mag = std::sqrt(gx * gx + gy * gy + gz * gz);
  std::sort(intervals_ms.begin(), intervals_ms.end());
  const double median = intervals_ms.empty() ? 0.0 : intervals_ms[intervals_ms.size() / 2];
  int outside = 0;
  for (double ms : intervals_ms) {
    if (ms < 4.0 || ms > 25.0) {
      ++outside;
    }
  }
  const double outside_frac = intervals_ms.empty()
                                  ? 1.0
                                  : static_cast<double>(outside) / static_cast<double>(intervals_ms.size());
  const double one_frac = steps == 0 ? 0.0 : static_cast<double>(steps_of_one) / static_cast<double>(steps);

  std::println("samples {}  sequence {}..{}  median interval {:.2f} ms", samples.size(),
               samples.front().sequence, samples.back().sequence, median);
  std::println("mean g {:.3f} {:.3f} {:.3f}  |g| {:.3f}", gx, gy, gz, mag);
  std::println("consecutive +1 steps {:.0f}%  intervals outside 4..25 ms {:.0f}%", one_frac * 100.0,
               outside_frac * 100.0);

  if (!expect(ordered, "sequence and timestamps advance", "a sample moved backwards")) {
    ++failures;
  }
  if (!expect(one_frac >= 0.90, "one interrupt per sample", "too many skipped sequence numbers")) {
    ++failures;
  }
  if (!expect(median >= 7.0 && median <= 16.0, "100 Hz data-ready", "median interval is not near 10 ms")) {
    ++failures;
  }
  if (!expect(outside_frac <= 0.10, "interval stability", "too many intervals far from 10 ms")) {
    ++failures;
  }
  if (!expect(mag >= 0.70 && mag <= 1.30, "gravity magnitude", "mean magnitude is outside 0.70..1.30 g")) {
    ++failures;
  }

  const unsigned long dropped_mid = read_counter("dropped");
  const unsigned long irq_mid = read_counter("irq_count");
  std::this_thread::sleep_for(300ms);
  const unsigned long dropped_after = read_counter("dropped");
  const unsigned long irq_after = read_counter("irq_count");
  const unsigned long errors_after = read_counter("i2c_errors");
  const unsigned long overruns = read_counter("overruns");

  std::println("irq_count {} -> {} during the pause", irq_mid, irq_after);
  std::println("dropped {} -> {}  overruns {}  i2c_errors {}", dropped_mid, dropped_after, overruns,
               errors_after);
  if (!expect(irq_after >= irq_mid + 20, "interrupts continue while userspace waits",
              "irq_count did not advance during a 300 ms pause")) {
    ++failures;
  }
  if (!expect(dropped_after >= dropped_mid + 10, "unread samples are counted",
              "dropped did not advance while reads were paused")) {
    ++failures;
  }
  if (!expect(errors_after == errors_before, "I2C errors", "i2c_errors increased")) {
    ++failures;
  }
  if (!expect(overruns <= irq_after / 2, "chip overruns", "OVERRUN is a large fraction of interrupts")) {
    ++failures;
  }
  if (!expect(dropped_after_reads <= dropped_before + 5, "kept up while reading",
              "dropped climbed while reads were blocking on each sample")) {
    ++failures;
  }

  adxl345_custom_sample again{};
  const ssize_t resumed = ::read(device.fd, &again, sizeof(again));
  if (!expect(resumed == static_cast<ssize_t>(sizeof(again)) && again.sequence > samples.back().sequence,
              "reads resume", "the read after the pause did not return a newer sample")) {
    ++failures;
  }

  if (failures != 0) {
    std::println("result: FAIL ({} checks)", failures);
    return 1;
  }
  std::println("result: PASS");
  return 0;
}
