// Hardware check that adxl345_custom's polled read and its GPIO17
// data-ready path agree while the board sits still.
// Loads kernel/adxl345_custom.ko twice: irq_gpio=-1, then the default GPIO17.
// Leaves the module loaded in interrupt mode. Run as root.

#include "adxl345/custom_mod_abi.h"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
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
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr int kSamples = 32;
constexpr double kLsbPerG = 256.0;
constexpr double kAxisTolG = 0.15;
constexpr double kMagTolG = 0.10;
constexpr double kGravityMin = 0.70;
constexpr double kGravityMax = 1.30;
constexpr const char* kSysfs = "/sys/bus/i2c/devices/1-0053";
constexpr const char* kDevice = "/dev/adxl345_custom";

struct CmdResult {
  int status = -1;
  std::string out;
};

struct Mean {
  double x = 0;
  double y = 0;
  double z = 0;
  [[nodiscard]] double mag() const { return std::sqrt(x * x + y * y + z * z); }
};

struct Series {
  Mean mean;
  double median_ms = 0;
  double max_read_ms = 0;
  double one_frac = 0;
  bool ordered = false;
  adxl345_u32 first_seq = 0;
  adxl345_u32 last_seq = 0;
};

struct Fd {
  int fd = -1;
  ~Fd() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  Fd() = default;
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};

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

[[nodiscard]] std::string read_text(const fs::path& path) {
  std::ifstream input(path);
  if (!input) {
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return trim(buffer.str());
}

[[nodiscard]] unsigned long read_counter(const char* name) {
  try {
    return std::stoul(read_text(fs::path(kSysfs) / name));
  } catch (const std::exception&) {
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

[[nodiscard]] CmdResult run(const std::vector<std::string>& args) {
  CmdResult result;
  if (args.empty()) {
    return result;
  }
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    result.out = "pipe failed";
    return result;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    result.out = "fork failed";
    return result;
  }
  if (pid == 0) {
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  ::close(pipefd[1]);
  char buf[4096];
  while (true) {
    const ssize_t got = ::read(pipefd[0], buf, sizeof(buf));
    if (got > 0) {
      result.out.append(buf, static_cast<std::size_t>(got));
    } else if (got == 0 || errno != EINTR) {
      break;
    }
  }
  ::close(pipefd[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  result.status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  return result;
}

[[nodiscard]] fs::path tool(std::string_view name) {
  for (const char* dir : {"/usr/sbin", "/sbin", "/usr/bin", "/bin"}) {
    const fs::path path = fs::path(dir) / std::string(name);
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
      return path;
    }
  }
  return fs::path(std::string(name));
}

[[nodiscard]] bool module_loaded(std::string_view name) {
  const CmdResult listed = run({tool("lsmod").string()});
  std::istringstream input(listed.out);
  std::string line;
  while (std::getline(input, line)) {
    const auto gap = line.find(' ');
    const std::string first = gap == std::string::npos ? line : line.substr(0, gap);
    if (first == name) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::string bound_driver() {
  std::error_code ec;
  const fs::path link = fs::path(kSysfs) / "driver";
  if (!fs::is_symlink(link, ec)) {
    return {};
  }
  const fs::path target = fs::read_symlink(link, ec);
  if (ec) {
    return {};
  }
  return target.filename().string();
}

void rmmod_custom() {
  if (!module_loaded("adxl345_custom")) {
    return;
  }
  const CmdResult result = run({tool("rmmod").string(), "adxl345_custom"});
  if (result.status != 0) {
    std::println(std::cerr, "rmmod adxl345_custom failed: {}", trim(result.out));
  }
}

[[nodiscard]] bool insmod_custom(const fs::path& ko, const std::string& param) {
  std::vector<std::string> args{tool("insmod").string(), ko.string()};
  if (!param.empty()) {
    args.push_back(param);
  }
  const CmdResult inserted = run(args);
  if (inserted.status != 0) {
    std::println(std::cerr, "insmod {} {} failed: {}", ko.string(), param, trim(inserted.out));
    return false;
  }
  return true;
}

[[nodiscard]] bool wait_for_device() {
  for (int i = 0; i < 40; ++i) {
    if (::access(kDevice, F_OK) == 0 && !read_text(fs::path(kSysfs) / "mode").empty()) {
      return true;
    }
    std::this_thread::sleep_for(50ms);
  }
  return false;
}

[[nodiscard]] bool open_device(Fd& device) {
  device.fd = -1;
  for (int i = 0; i < 40 && device.fd < 0; ++i) {
    device.fd = ::open(kDevice, O_RDONLY);
    if (device.fd < 0) {
      std::this_thread::sleep_for(50ms);
    }
  }
  if (device.fd < 0) {
    std::println(std::cerr, "open {}: {}", kDevice, std::strerror(errno));
    return false;
  }
  return true;
}

void print_mean(std::string_view label, const Mean& mean) {
  std::println("{:22} {:>8.3f} {:>8.3f} {:>8.3f} {:>8.3f}", label, mean.x, mean.y, mean.z, mean.mag());
}

[[nodiscard]] bool take_series(int fd, bool pace, Series& series) {
  std::vector<adxl345_custom_sample> samples;
  samples.reserve(static_cast<std::size_t>(kSamples));
  double max_read_ms = 0;
  for (int i = 0; i < kSamples; ++i) {
    adxl345_custom_sample sample{};
    const auto started = std::chrono::steady_clock::now();
    const ssize_t got = ::read(fd, &sample, sizeof(sample));
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    max_read_ms = std::max(max_read_ms, elapsed_ms);
    if (got != static_cast<ssize_t>(sizeof(sample))) {
      std::println("FAIL  read {} returned {} ({})", i, got, std::strerror(errno));
      return false;
    }
    if (sample.lsb_per_g != ADXL345_CUSTOM_LSB_PER_G) {
      std::println("FAIL  sample scale is {} LSB/g", sample.lsb_per_g);
      return false;
    }
    samples.push_back(sample);
    if (pace && i + 1 < kSamples) {
      std::this_thread::sleep_for(10ms);
    }
  }

  std::vector<double> intervals_ms;
  int steps = 0;
  int steps_of_one = 0;
  bool ordered = true;
  double sx = 0;
  double sy = 0;
  double sz = 0;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    sx += samples[i].x;
    sy += samples[i].y;
    sz += samples[i].z;
    if (i == 0) {
      continue;
    }
    ++steps;
    if (samples[i].sequence == samples[i - 1].sequence + 1) {
      ++steps_of_one;
    } else if (samples[i].sequence <= samples[i - 1].sequence) {
      ordered = false;
    }
    if (samples[i].timestamp_ns <= samples[i - 1].timestamp_ns) {
      ordered = false;
    }
    intervals_ms.push_back(static_cast<double>(samples[i].timestamp_ns - samples[i - 1].timestamp_ns) / 1.0e6);
  }
  const double count = static_cast<double>(samples.size());
  series.mean.x = (sx / count) / kLsbPerG;
  series.mean.y = (sy / count) / kLsbPerG;
  series.mean.z = (sz / count) / kLsbPerG;
  std::sort(intervals_ms.begin(), intervals_ms.end());
  series.median_ms = intervals_ms.empty() ? 0.0 : intervals_ms[intervals_ms.size() / 2];
  series.max_read_ms = max_read_ms;
  series.one_frac = steps == 0 ? 0.0 : static_cast<double>(steps_of_one) / static_cast<double>(steps);
  series.ordered = ordered;
  series.first_seq = samples.front().sequence;
  series.last_seq = samples.back().sequence;
  std::println("samples {}  sequence {}..{}  median interval {:.2f} ms  slowest read {:.2f} ms", samples.size(),
               series.first_seq, series.last_seq, series.median_ms, series.max_read_ms);
  std::println("mean g {:.3f} {:.3f} {:.3f}  |g| {:.3f}", series.mean.x, series.mean.y, series.mean.z,
               series.mean.mag());
  return true;
}

[[nodiscard]] bool gravity_ok(const Mean& mean) {
  const double mag = mean.mag();
  return mag >= kGravityMin && mag <= kGravityMax;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2) {
    std::println(std::cerr, "usage: adxl345_modes [repo-root]");
    return 3;
  }
  if (geteuid() != 0) {
    std::println(std::cerr, "run as root so insmod can load adxl345_custom");
    return 3;
  }

  const fs::path root = fs::absolute(argc == 2 ? argv[1] : ".");
  const fs::path ko = root / "kernel" / "adxl345_custom.ko";
  std::error_code ec;
  if (!fs::is_regular_file(ko, ec)) {
    std::println(std::cerr, "missing {} (build it with make -C kernel)", ko.string());
    return 2;
  }

  const std::string owner = bound_driver();
  if (!owner.empty() && owner != "adxl345_custom") {
    std::println(std::cerr, "address 0x53 is owned by {}", owner);
    return 2;
  }

  const CmdResult kernel = run({tool("uname").string(), "-r"});
  std::println("ADXL345 poll and interrupt comparison");
  std::println("kernel: {}", trim(kernel.out));
  std::println("module: {}", ko.string());

  rmmod_custom();
  if (module_loaded("adxl345_custom")) {
    return 2;
  }
  if (!insmod_custom(ko, "irq_gpio=-1") || !wait_for_device()) {
    std::println(std::cerr, "poll module did not bind");
    rmmod_custom();
    if (!insmod_custom(ko, "")) {
      std::println(std::cerr, "could not reload adxl345_custom on GPIO17");
    }
    return 2;
  }

  // If a later check returns, put the GPIO17 driver back.
  struct Restore {
    fs::path module;
    bool armed = true;
    ~Restore() {
      if (!armed) {
        return;
      }
      rmmod_custom();
      if (!insmod_custom(module, "")) {
        std::println(std::cerr, "could not reload adxl345_custom on GPIO17");
      }
    }
  } restore{ko, true};

  int failures = 0;
  Series polled;
  {
    const std::string mode = read_text(fs::path(kSysfs) / "mode");
    const std::string gpio = read_text(fs::path(kSysfs) / "irq_gpio");
    const std::string line = read_text(fs::path(kSysfs) / "int_line");
    const std::string trigger = read_text(fs::path(kSysfs) / "trigger");
    std::println("\n== poll (irq_gpio=-1) ==");
    std::println("mode {}  gpio {}  int_line {}  trigger {}", mode, gpio, line, trigger);
    if (!expect(mode == "poll" && gpio == "-1" && line == "0" && trigger == "none", "poll sysfs",
                "mode, gpio, int_line, or trigger did not describe a polled module")) {
      ++failures;
    }

    Fd device;
    if (!open_device(device)) {
      return 2;
    }
    pollfd ready{};
    ready.fd = device.fd;
    ready.events = POLLIN;
    const int ready_now = ::poll(&ready, 1, 0);
    if (!expect(ready_now == 1 && (ready.revents & POLLIN) != 0, "poll read is ready immediately",
                "poll() on the device did not report readable data")) {
      ++failures;
    }

    const unsigned long irq_before = read_counter("irq_count");
    const unsigned long dropped_before = read_counter("dropped");
    const unsigned long errors_before = read_counter("i2c_errors");
    if (!take_series(device.fd, true, polled)) {
      return 1;
    }
    const unsigned long irq_after = read_counter("irq_count");
    const unsigned long dropped_after = read_counter("dropped");
    const unsigned long errors_after = read_counter("i2c_errors");
    std::println("irq_count {}  dropped {}  i2c_errors {}", irq_after, dropped_after, errors_after);

    if (!expect(polled.max_read_ms < 50.0, "polled read returns without the IRQ wait",
                "a read took 50 ms or more")) {
      ++failures;
    }
    if (!expect(polled.ordered && polled.one_frac == 1.0, "one sample per polled read",
                "sequence did not advance by 1 on every read")) {
      ++failures;
    }
    if (!expect(polled.median_ms >= 8.0 && polled.median_ms <= 30.0, "reads follow the 100 Hz output",
                "median timestamp gap is outside 8..30 ms")) {
      ++failures;
    }
    if (!expect(gravity_ok(polled.mean), "poll gravity", "mean magnitude is outside 0.70..1.30 g")) {
      ++failures;
    }
    if (!expect(irq_before == 0 && irq_after == 0, "poll takes no GPIO interrupt",
                "irq_count moved while irq_gpio was -1")) {
      ++failures;
    }
    if (!expect(dropped_before == 0 && dropped_after == 0, "polled reads are not drops",
                "dropped moved; each read is supposed to consume the sample it publishes")) {
      ++failures;
    }
    if (!expect(errors_before == 0 && errors_after == 0, "poll I2C errors", "i2c_errors increased")) {
      ++failures;
    }
  }

  rmmod_custom();
  if (module_loaded("adxl345_custom") || !insmod_custom(ko, "") || !wait_for_device()) {
    std::println(std::cerr, "GPIO17 module did not bind");
    return 2;
  }
  restore.armed = false;

  Series irq;
  {
    const std::string mode = read_text(fs::path(kSysfs) / "mode");
    const std::string gpio = read_text(fs::path(kSysfs) / "irq_gpio");
    const std::string line = read_text(fs::path(kSysfs) / "int_line");
    const std::string trigger = read_text(fs::path(kSysfs) / "trigger");
    std::println("\n== interrupt (GPIO17) ==");
    std::println("mode {}  gpio {}  int_line {}  trigger {}", mode, gpio, line, trigger);
    if (!expect(mode == "irq" && gpio == "17" && (line == "1" || line == "2") &&
                    (trigger == "level-high" || trigger == "rising"),
                "interrupt sysfs", "module did not stay on the GPIO17 data-ready interrupt")) {
      ++failures;
    }

    Fd device;
    if (!open_device(device)) {
      return 2;
    }
    const unsigned long errors_before = read_counter("i2c_errors");
    if (!take_series(device.fd, false, irq)) {
      return 1;
    }
    const unsigned long errors_after = read_counter("i2c_errors");
    if (!expect(irq.ordered && irq.one_frac >= 0.90, "one interrupt per sample",
                "too many skipped sequence numbers")) {
      ++failures;
    }
    if (!expect(irq.median_ms >= 7.0 && irq.median_ms <= 16.0, "100 Hz data-ready",
                "median interval is not near 10 ms")) {
      ++failures;
    }
    if (!expect(gravity_ok(irq.mean), "interrupt gravity", "mean magnitude is outside 0.70..1.30 g")) {
      ++failures;
    }
    if (!expect(errors_after == errors_before, "interrupt I2C errors", "i2c_errors increased")) {
      ++failures;
    }
  }

  const double dx = std::abs(polled.mean.x - irq.mean.x);
  const double dy = std::abs(polled.mean.y - irq.mean.y);
  const double dz = std::abs(polled.mean.z - irq.mean.z);
  const double dm = std::abs(polled.mean.mag() - irq.mean.mag());
  Mean delta;
  delta.x = polled.mean.x - irq.mean.x;
  delta.y = polled.mean.y - irq.mean.y;
  delta.z = polled.mean.z - irq.mean.z;

  std::println("\n== same board ==");
  std::println("{:22} {:>8} {:>8} {:>8} {:>8}", "", "x g", "y g", "z g", "|g|");
  print_mean("poll (irq_gpio=-1)", polled.mean);
  print_mean("irq (GPIO17)", irq.mean);
  print_mean("delta (poll - irq)", delta);
  std::println("per-axis tolerance: {:.2f} g    magnitude tolerance: {:.2f} g", kAxisTolG, kMagTolG);
  std::println("largest axis gap: {:.3f} g    magnitude gap: {:.3f} g", std::max(dx, std::max(dy, dz)), dm);

  if (!expect(dx <= kAxisTolG && dy <= kAxisTolG && dz <= kAxisTolG, "poll matches interrupt per axis",
              "an axis differs by more than 0.15 g")) {
    ++failures;
  }
  if (!expect(dm <= kMagTolG, "poll matches interrupt magnitude",
              "magnitudes differ by more than 0.10 g")) {
    ++failures;
  }

  if (failures != 0) {
    std::println("result: FAIL ({} checks)", failures);
    return 1;
  }
  std::println("result: PASS");
  return 0;
}
