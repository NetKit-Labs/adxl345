// Linux-only C++23 check. Loads the in-tree input driver, the IIO driver built
// out of tree from the rpi-6.18.y sources, and kernel/adxl345_custom.ko.
// Samples are sequential: one driver owns address 0x53 at a time.

#include "adxl345/custom_mod_abi.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <format>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>
#include <sys/wait.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr int kBus = 1;
constexpr unsigned kAddr = 0x53;
constexpr int kSamples = 32;
constexpr double kLsbPerG = 256.0;
constexpr double kGravity = 9.80665;
constexpr double kAxisTolG = 0.15;
constexpr double kMagTolG = 0.10;
constexpr double kGravityMin = 0.70;
constexpr double kGravityMax = 1.30;

struct CmdResult {
  int status = -1;
  std::string out;
};

struct Sample {
  double x = 0;
  double y = 0;
  double z = 0;
};

struct Mean {
  double x = 0;
  double y = 0;
  double z = 0;
  int n = 0;
  [[nodiscard]] double mag() const { return std::sqrt(x * x + y * y + z * z); }
};

struct Context {
  fs::path root;
  bool keep_custom = false;
  bool overlay_on = false;
  std::string input_overlay;
};

[[nodiscard]] std::string trim(std::string text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.pop_back();
  }
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  return text.substr(begin);
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
    close(pipefd[0]);
    close(pipefd[1]);
    result.out = "fork failed";
    return result;
  }
  if (pid == 0) {
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(pipefd[1]);
  char buf[4096];
  while (true) {
    const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
    if (n > 0) {
      result.out.append(buf, static_cast<std::size_t>(n));
    } else if (n == 0) {
      break;
    } else if (errno != EINTR) {
      break;
    }
  }
  close(pipefd[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  result.status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  return result;
}

[[nodiscard]] fs::path tool(std::string_view name) {
  for (const char* dir : {"/usr/sbin", "/sbin", "/usr/bin", "/bin"}) {
    fs::path path = fs::path(dir) / std::string(name);
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

void rmmod(std::string_view name) {
  if (!module_loaded(name)) {
    return;
  }
  const CmdResult result = run({tool("rmmod").string(), std::string(name)});
  if (result.status != 0) {
    std::println(std::cerr, "rmmod {} failed: {}", name, trim(result.out));
  }
}

[[nodiscard]] fs::path client_path() {
  return fs::path("/sys/bus/i2c/devices") / std::format("{}-{:04x}", kBus, kAddr);
}

[[nodiscard]] fs::path adapter_path() {
  return fs::path("/sys/bus/i2c/devices") / std::format("i2c-{}", kBus);
}

[[nodiscard]] std::string read_text(const fs::path& path) {
  std::ifstream input(path);
  if (!input) {
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

[[nodiscard]] std::optional<double> read_number(const fs::path& path) {
  const std::string text = trim(read_text(path));
  if (text.empty()) {
    return std::nullopt;
  }
  try {
    std::size_t used = 0;
    const double value = std::stod(text, &used);
    if (used == 0) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

[[nodiscard]] bool write_sysfs(const fs::path& path, std::string data, std::string& error) {
  if (data.empty() || data.back() != '\n') {
    data.push_back('\n');
  }
  const int fd = ::open(path.c_str(), O_WRONLY);
  if (fd < 0) {
    error = std::strerror(errno);
    return false;
  }
  const ssize_t n = ::write(fd, data.data(), data.size());
  const int err = errno;
  ::close(fd);
  if (n < 0) {
    error = std::strerror(err);
    return false;
  }
  return true;
}

[[nodiscard]] std::string bound_driver() {
  const fs::path link = client_path() / "driver";
  std::error_code ec;
  if (!fs::is_symlink(link, ec)) {
    return {};
  }
  const fs::path target = fs::read_symlink(link, ec);
  if (ec) {
    return {};
  }
  return target.filename().string();
}

void delete_client() {
  std::error_code ec;
  if (!fs::exists(client_path(), ec)) {
    return;
  }
  std::string error;
  if (!write_sysfs(adapter_path() / "delete_device", std::format("0x{:x}", kAddr), error)) {
    std::println(std::cerr, "delete_device 0x{:02x} failed: {}", kAddr, error);
    return;
  }
  std::this_thread::sleep_for(50ms);
}

[[nodiscard]] std::vector<std::string> adxl_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    if (line.find("adxl") != std::string::npos || line.find("ADXL") != std::string::npos) {
      lines.push_back(line);
    }
  }
  return lines;
}

void print_new_kernel(const std::vector<std::string>& before,
                      const std::vector<std::string>& after) {
  std::size_t start = 0;
  if (after.size() >= before.size()) {
    bool prefix = true;
    for (std::size_t i = 0; i < before.size(); ++i) {
      if (before[i] != after[i]) {
        prefix = false;
        break;
      }
    }
    if (prefix) {
      start = before.size();
    }
  }
  if (start >= after.size()) {
    std::println("kernel: no new adxl lines");
    return;
  }
  std::println("kernel:");
  for (std::size_t i = start; i < after.size(); ++i) {
    std::println("  {}", after[i]);
  }
}

[[nodiscard]] std::string dmesg() { return run({tool("dmesg").string()}).out; }

void drop_overlay(Context& ctx) {
  if (!ctx.overlay_on) {
    return;
  }
  const CmdResult result =
      run({tool("dtoverlay").string(), "-r", "adxl345-compare"});
  ctx.overlay_on = false;
  if (result.status != 0) {
    std::println(std::cerr, "dtoverlay -r failed: {}", trim(result.out));
  }
  std::this_thread::sleep_for(100ms);
}

void drop_input_overlay(Context& ctx) {
  if (ctx.input_overlay.empty()) {
    return;
  }
  const CmdResult result = run({tool("dtoverlay").string(), "-r", ctx.input_overlay});
  if (result.status != 0) {
    std::println(std::cerr, "dtoverlay -r {} failed: {}", ctx.input_overlay, trim(result.out));
  }
  ctx.input_overlay.clear();
  std::this_thread::sleep_for(200ms);
}

void release_bus(Context& ctx) {
  drop_input_overlay(ctx);
  drop_overlay(ctx);
  rmmod("adxl345_custom");
  std::error_code ec;
  if (fs::exists(client_path(), ec) && bound_driver() != "adxl345_custom") {
    delete_client();
  }
  rmmod("adxl345_i2c");
  rmmod("adxl345_core");
  rmmod("adxl34x_i2c");
  rmmod("adxl34x");
}

[[nodiscard]] Mean summarize(const std::vector<Sample>& samples) {
  Mean mean;
  mean.n = static_cast<int>(samples.size());
  if (samples.empty()) {
    return mean;
  }
  for (const Sample& sample : samples) {
    mean.x += sample.x;
    mean.y += sample.y;
    mean.z += sample.z;
  }
  const double n = static_cast<double>(mean.n);
  mean.x /= n;
  mean.y /= n;
  mean.z /= n;
  return mean;
}

void print_mean(std::string_view label, const Mean& mean, double lsb) {
  const double gx = mean.x / lsb;
  const double gy = mean.y / lsb;
  const double gz = mean.z / lsb;
  std::println("{:22} {:8.1f} {:8.1f} {:8.1f} {:8.1f} {:8.3f} {:8.3f} {:8.3f} {:8.3f}",
               label, mean.x, mean.y, mean.z, mean.mag(), gx, gy, gz,
               std::sqrt(gx * gx + gy * gy + gz * gz));
}

[[nodiscard]] std::string compiler_dialect(const fs::path& root) {
  std::ifstream input(root / "kernel" / ".adxl345_custom.o.cmd");
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  const auto pos = text.find("-std=");
  if (pos == std::string::npos) {
    return "(not recorded)";
  }
  const auto end = text.find_first_of(" \n\t", pos);
  return text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

[[nodiscard]] bool apply_local_input_overlay(Context& ctx) {
  const fs::path dtc = tool("dtc");
  const fs::path dtoverlay = tool("dtoverlay");
  std::error_code ec;
  const fs::path dts = ctx.root / "kernel" / "adxl34x-overlay.dts";
  if (!fs::is_regular_file(dtc, ec) || !fs::is_regular_file(dtoverlay, ec) ||
      !fs::is_regular_file(dts, ec)) {
    std::println("local adxl34x overlay is unavailable");
    return false;
  }
  const fs::path dtbo = "/tmp/adxl34x-irq.dtbo";
  const CmdResult built = run({dtc.string(), "-@", "-I", "dts", "-O", "dtb", "-o",
                               dtbo.string(), dts.string()});
  if (built.status != 0) {
    std::println("dtc failed: {}", trim(built.out));
    return false;
  }
  const CmdResult applied = run({dtoverlay.string(), "-d", "/tmp", "adxl34x-irq"});
  if (applied.status != 0) {
    std::println("dtoverlay adxl34x-irq failed: {}", trim(applied.out));
    return false;
  }
  ctx.input_overlay = "adxl34x-irq";
  std::this_thread::sleep_for(400ms);
  return true;
}

[[nodiscard]] fs::path find_input_event() {
  std::error_code ec;
  const fs::path root = "/sys/class/input";
  if (!fs::exists(root, ec)) {
    return {};
  }
  for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
    const std::string filename = entry.path().filename().string();
    if (!filename.starts_with("event")) {
      continue;
    }
    const std::string name = trim(read_text(entry.path() / "device" / "name"));
    if (name == "ADXL34x accelerometer") {
      return fs::path("/dev/input") / filename;
    }
  }
  return {};
}

[[nodiscard]] bool read_position(Sample& sample) {
  const std::string text = trim(read_text(client_path() / "position"));
  int x = 0;
  int y = 0;
  int z = 0;
  if (std::sscanf(text.c_str(), "(%d, %d, %d)", &x, &y, &z) != 3) {
    return false;
  }
  sample.x = static_cast<double>(x);
  sample.y = static_cast<double>(y);
  sample.z = static_cast<double>(z);
  return true;
}

void print_irq_lines() {
  std::ifstream input("/proc/interrupts");
  std::string line;
  while (std::getline(input, line)) {
    if (line.find("adxl") != std::string::npos || line.find("1-0053") != std::string::npos) {
      std::println("irq: {}", trim(line));
    }
  }
}

[[nodiscard]] bool test_input(Context& ctx, std::vector<Sample>& samples) {
  std::println("\n== shipped input driver (adxl34x) with GPIO17 ==");
  samples.clear();
  const auto before = adxl_lines(dmesg());
  const CmdResult probed = run({tool("modprobe").string(), "adxl34x-i2c"});
  if (probed.status != 0) {
    std::println("modprobe adxl34x-i2c failed: {}", trim(probed.out));
    return false;
  }
  /* The image i2c-sensor,adxl345 overlay leaves interrupt-parent unresolved,
   * so adxl34x still probes with irq 0. This overlay only names GPIO17. */
  if (!apply_local_input_overlay(ctx)) {
    return false;
  }
  const std::string driver = bound_driver();
  print_new_kernel(before, adxl_lines(dmesg()));
  std::println("overlay: {}", ctx.input_overlay);
  std::println("bound driver: {}", driver.empty() ? "(none)" : driver);
  std::println("rate code: {}", trim(read_text(client_path() / "rate")).empty()
                                   ? "(missing)"
                                   : trim(read_text(client_path() / "rate")));
  if (driver != "adxl34x") {
    std::println("result: adxl34x did not bind with GPIO17 as its interrupt");
    return false;
  }

  fs::path event;
  for (int i = 0; i < 40 && event.empty(); ++i) {
    event = find_input_event();
    if (event.empty()) {
      std::this_thread::sleep_for(50ms);
    }
  }
  if (event.empty()) {
    std::println("ADXL34x accelerometer input node was not created");
    return false;
  }
  const int fd = ::open(event.c_str(), O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    std::println("open {} failed: {}", event.string(), std::strerror(errno));
    return false;
  }
  std::println("holding {} open so the driver leaves measure mode enabled", event.string());
  std::this_thread::sleep_for(150ms);
  for (int i = 0; i < kSamples; ++i) {
    Sample sample;
    if (!read_position(sample)) {
      std::println("position sysfs was not a sample");
      ::close(fd);
      return false;
    }
    samples.push_back(sample);
    std::this_thread::sleep_for(50ms);
  }
  print_irq_lines();
  ::close(fd);
  if (samples.empty()) {
    return false;
  }
  const Mean mean = summarize(samples);
  std::println("samples: {}  first raw {:g} {:g} {:g}  mean raw {:g} {:g} {:g}",
               samples.size(), samples.front().x, samples.front().y, samples.front().z,
               mean.x, mean.y, mean.z);
  const bool live = mean.mag() > 1.0;
  std::println("result: {}", live ? "live samples from the image driver" : "position stayed near zero");
  return live;
}

[[nodiscard]] bool apply_overlay(Context& ctx) {
  const fs::path dtc = tool("dtc");
  const fs::path dtoverlay = tool("dtoverlay");
  std::error_code ec;
  if (!fs::is_regular_file(dtc, ec) || !fs::is_regular_file(dtoverlay, ec)) {
    std::println("dtc or dtoverlay is missing; overlay bind skipped");
    return false;
  }
  const fs::path dts = ctx.root / "kernel" / "adxl345-overlay.dts";
  const fs::path dtbo = "/tmp/adxl345-compare.dtbo";
  const CmdResult built = run({dtc.string(), "-@", "-I", "dts", "-O", "dtb", "-o",
                               dtbo.string(), dts.string()});
  if (built.status != 0) {
    std::println("dtc failed: {}", trim(built.out));
    return false;
  }
  const CmdResult applied =
      run({dtoverlay.string(), "-d", "/tmp", "adxl345-compare"});
  if (applied.status != 0) {
    std::println("dtoverlay failed: {}", trim(applied.out));
    return false;
  }
  ctx.overlay_on = true;
  std::this_thread::sleep_for(300ms);
  return true;
}

[[nodiscard]] fs::path find_iio() {
  std::error_code ec;
  const fs::path root = "/sys/bus/iio/devices";
  if (!fs::exists(root, ec)) {
    return {};
  }
  for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
    if (trim(read_text(entry.path() / "name")) == "adxl345") {
      return entry.path();
    }
  }
  return {};
}

[[nodiscard]] bool read_iio(const fs::path& device, std::vector<Sample>& samples,
                            double& scale) {
  const std::optional<double> read_scale = read_number(device / "in_accel_scale");
  if (!read_scale) {
    std::println("in_accel_scale is missing under {}", device.string());
    return false;
  }
  scale = *read_scale;
  for (const char* axis : {"x", "y", "z"}) {
    const fs::path bias = device / std::format("in_accel_{}_calibbias", axis);
    std::error_code ec;
    if (!fs::exists(bias, ec)) {
      continue;
    }
    std::string error;
    if (!write_sysfs(bias, "0", error)) {
      std::println("could not zero {}: {}", bias.string(), error);
    }
  }
  std::this_thread::sleep_for(50ms);
  samples.clear();
  for (int i = 0; i < kSamples; ++i) {
    const std::optional<double> x = read_number(device / "in_accel_x_raw");
    const std::optional<double> y = read_number(device / "in_accel_y_raw");
    const std::optional<double> z = read_number(device / "in_accel_z_raw");
    if (!x || !y || !z) {
      std::println("IIO raw read failed on sample {}", i);
      return false;
    }
    samples.push_back(Sample{*x, *y, *z});
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

[[nodiscard]] bool test_iio(Context& ctx, std::vector<Sample>& samples, double& scale) {
  std::println("\n== pre-existing IIO driver (adxl345_i2c, built out of tree) ==");
  for (const char* name : {"industrialio", "regmap-i2c", "kfifo_buf",
                           "industrialio-triggered-buffer"}) {
    (void)run({tool("modprobe").string(), name});
  }
  const fs::path core = ctx.root / "kernel" / "stock" / "adxl345_core.ko";
  const fs::path i2c = ctx.root / "kernel" / "stock" / "adxl345_i2c.ko";
  const auto before = adxl_lines(dmesg());
  const CmdResult core_in = run({tool("insmod").string(), core.string()});
  if (core_in.status != 0) {
    std::println("insmod {} failed: {}", core.string(), trim(core_in.out));
    print_new_kernel(before, adxl_lines(dmesg()));
    return false;
  }
  const CmdResult i2c_in = run({tool("insmod").string(), i2c.string()});
  if (i2c_in.status != 0) {
    std::println("insmod {} failed: {}", i2c.string(), trim(i2c_in.out));
    print_new_kernel(before, adxl_lines(dmesg()));
    return false;
  }
  std::string error;
  const bool wrote = write_sysfs(adapter_path() / "new_device",
                                 std::format("adxl345 0x{:x}", kAddr), error);
  std::this_thread::sleep_for(300ms);
  if (bound_driver() != "adxl345_i2c") {
    std::println("sysfs bind did not attach adxl345_i2c ({})",
                 wrote ? "write ok, driver not bound" : error);
    delete_client();
    std::println("trying a device-tree overlay for compatible adi,adxl345");
    if (!apply_overlay(ctx)) {
      print_new_kernel(before, adxl_lines(dmesg()));
      return false;
    }
  }
  print_new_kernel(before, adxl_lines(dmesg()));
  const fs::path device = find_iio();
  if (device.empty()) {
    std::println("no iio device named adxl345. bound driver: {}",
                 bound_driver().empty() ? "(none)" : bound_driver());
    return false;
  }
  std::println("iio device: {}", device.string());
  std::println("bound driver: {}", bound_driver().empty() ? "(none)" : bound_driver());
  if (!read_iio(device, samples, scale)) {
    return false;
  }
  const Mean mean = summarize(samples);
  std::println("samples: {}  first raw {:g} {:g} {:g}  mean raw {:g} {:g} {:g}",
               samples.size(), samples.front().x, samples.front().y, samples.front().z,
               mean.x, mean.y, mean.z);
  std::println("published in_accel_scale: {:.6f} m/s^2 per LSB", scale);
  drop_overlay(ctx);
  delete_client();
  rmmod("adxl345_i2c");
  rmmod("adxl345_core");
  return true;
}

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

[[nodiscard]] bool test_custom(Context& ctx, std::vector<Sample>& samples, int& lsb) {
  std::println("\n== new module (adxl345_custom) ==");
  const fs::path ko = ctx.root / "kernel" / "adxl345_custom.ko";
  const auto before = adxl_lines(dmesg());
  const CmdResult inserted = run({tool("insmod").string(), ko.string()});
  if (inserted.status != 0) {
    std::println("insmod {} failed: {}", ko.string(), trim(inserted.out));
    print_new_kernel(before, adxl_lines(dmesg()));
    return false;
  }
  ctx.keep_custom = true;
  print_new_kernel(before, adxl_lines(dmesg()));
  Fd device;
  for (int i = 0; i < 40 && device.fd < 0; ++i) {
    device.fd = ::open("/dev/adxl345_custom", O_RDONLY);
    if (device.fd < 0) {
      std::this_thread::sleep_for(50ms);
    }
  }
  if (device.fd < 0) {
    std::println("open /dev/adxl345_custom failed: {}", std::strerror(errno));
    return false;
  }
  const std::string devid = trim(read_text(client_path() / "devid"));
  const std::string lsb_text = trim(read_text(client_path() / "lsb_per_g"));
  const std::string driver = bound_driver();
  std::println("bound driver: {}", driver.empty() ? "(none)" : driver);
  std::println("sysfs devid: {}", devid.empty() ? "(missing)" : devid);
  std::println("sysfs lsb_per_g: {}", lsb_text.empty() ? "(missing)" : lsb_text);
  if (devid != "0xe5" && devid != "0xE5") {
    std::println("DEVID is not 0xe5");
    return false;
  }
  std::this_thread::sleep_for(50ms);
  samples.clear();
  for (int i = 0; i < kSamples; ++i) {
    adxl345_custom_sample sample{};
    const ssize_t n = ::read(device.fd, &sample, sizeof(sample));
    if (n != static_cast<ssize_t>(sizeof(sample))) {
      std::println("read /dev/adxl345_custom returned {} ({})", n, std::strerror(errno));
      return false;
    }
    if (i == 0) {
      lsb = sample.lsb_per_g;
    }
    if (sample.lsb_per_g != lsb) {
      std::println("lsb_per_g changed mid-run");
      return false;
    }
    samples.push_back(Sample{static_cast<double>(sample.x), static_cast<double>(sample.y),
                             static_cast<double>(sample.z)});
    std::this_thread::sleep_for(5ms);
  }
  const Mean mean = summarize(samples);
  std::println("samples: {}  first raw {:g} {:g} {:g}  mean raw {:g} {:g} {:g}",
               samples.size(), samples.front().x, samples.front().y, samples.front().z,
               mean.x, mean.y, mean.z);
  std::println("char-device scale: {} LSB/g", lsb);
  return lsb == ADXL345_CUSTOM_LSB_PER_G;
}

void cleanup(Context& ctx) {
  drop_overlay(ctx);
  rmmod("adxl345_i2c");
  rmmod("adxl345_core");
  rmmod("adxl34x_i2c");
  rmmod("adxl34x");
  if (!ctx.keep_custom) {
    rmmod("adxl345_custom");
    if (bound_driver() != "adxl345_custom") {
      delete_client();
    }
  }
}

[[nodiscard]] bool close_enough(const Mean& left, const Mean& right) {
  const double lx = left.x / kLsbPerG;
  const double ly = left.y / kLsbPerG;
  const double lz = left.z / kLsbPerG;
  const double rx = right.x / kLsbPerG;
  const double ry = right.y / kLsbPerG;
  const double rz = right.z / kLsbPerG;
  const double lg = std::sqrt(lx * lx + ly * ly + lz * lz);
  const double rg = std::sqrt(rx * rx + ry * ry + rz * rz);
  return std::abs(lx - rx) <= kAxisTolG && std::abs(ly - ry) <= kAxisTolG &&
         std::abs(lz - rz) <= kAxisTolG && std::abs(lg - rg) <= kMagTolG &&
         lg >= kGravityMin && lg <= kGravityMax && rg >= kGravityMin && rg <= kGravityMax;
}

[[nodiscard]] int compare(const Mean& iio, const Mean& custom, double iio_scale,
                          const Mean& shipped) {
  const double ix = iio.x / kLsbPerG;
  const double iy = iio.y / kLsbPerG;
  const double iz = iio.z / kLsbPerG;
  const double cx = custom.x / kLsbPerG;
  const double cy = custom.y / kLsbPerG;
  const double cz = custom.z / kLsbPerG;
  const double ig = std::sqrt(ix * ix + iy * iy + iz * iz);
  const double cg = std::sqrt(cx * cx + cy * cy + cz * cz);
  const double datasheet = kGravity / kLsbPerG;
  const bool gravity_ok = ig >= kGravityMin && ig <= kGravityMax && cg >= kGravityMin &&
                          cg <= kGravityMax;
  const bool axis_ok = std::abs(ix - cx) <= kAxisTolG && std::abs(iy - cy) <= kAxisTolG &&
                       std::abs(iz - cz) <= kAxisTolG;
  const bool mag_ok = std::abs(ig - cg) <= kMagTolG;
  const bool shipped_ok = close_enough(shipped, custom);

  std::println("\n== comparison (both converted at {} LSB/g) ==", static_cast<int>(kLsbPerG));
  std::println("{:22} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8}", "", "raw x", "raw y",
               "raw z", "|raw|", "x g", "y g", "z g", "|g|");
  print_mean("adxl34x (image)", shipped, kLsbPerG);
  print_mean("IIO adxl345_i2c", iio, kLsbPerG);
  print_mean("adxl345_custom", custom, kLsbPerG);
  Mean delta;
  delta.x = iio.x - custom.x;
  delta.y = iio.y - custom.y;
  delta.z = iio.z - custom.z;
  delta.n = 1;
  print_mean("delta (IIO - custom)", delta, kLsbPerG);

  std::println("datasheet full-res scale: {:.6f} m/s^2 per LSB", datasheet);
  std::println("IIO published scale:      {:.6f} m/s^2 per LSB", iio_scale);
  if (std::abs(iio_scale - datasheet) / datasheet > 0.05) {
    const double px = iio.x * iio_scale / kGravity;
    const double py = iio.y * iio_scale / kGravity;
    const double pz = iio.z * iio_scale / kGravity;
    std::println(
        "IIO g using its published scale: {:.3f} {:.3f} {:.3f}  |g| {:.3f}",
        px, py, pz, std::sqrt(px * px + py * py + pz * pz));
    std::println(
        "The published scale is not the full-resolution 3.9 mg/LSB. "
        "Probe writes DATA_FORMAT 0x08 (full-res, +/-2 g), and at that setting "
        "one g is still about 256 counts. The pass/fail uses 256 LSB/g for both drivers.");
  }
  std::println("per-axis tolerance: {:.2f} g    magnitude tolerance: {:.2f} g", kAxisTolG,
               kMagTolG);
  std::println("gravity band: {:.2f} g to {:.2f} g", kGravityMin, kGravityMax);
  std::println("gravity band: {}", gravity_ok ? "pass" : "fail");
  std::println("IIO per-axis delta: {}", axis_ok ? "pass" : "fail");
  std::println("IIO magnitude delta: {}", mag_ok ? "pass" : "fail");
  std::println("image driver vs custom: {}", shipped_ok ? "pass" : "fail");
  if (gravity_ok && axis_ok && mag_ok && shipped_ok) {
    std::println("result: PASS");
    return 0;
  }
  std::println("result: FAIL");
  return 1;
}

[[nodiscard]] int compare_main(Context& ctx, int argc, char** argv) {
  if (argc > 2) {
    std::println(std::cerr, "usage: adxl345_compare [repo-root]");
    return 3;
  }
  if (geteuid() != 0) {
    std::println(std::cerr, "run as root so insmod and the i2c sysfs nodes work");
    return 3;
  }
  ctx.root = fs::absolute(argc == 2 ? argv[1] : ".");
  const CmdResult kernel = run({tool("uname").string(), "-r"});
  std::println("ADXL345 kernel driver comparison");
  std::println("kernel: {}", trim(kernel.out));
  std::println("module C dialect: {}", compiler_dialect(ctx.root));
  std::println("bus i2c-{} address 0x{:02x}", kBus, kAddr);
  std::println("samples: {} per driver, taken one driver at a time", kSamples);

  release_bus(ctx);
  std::vector<Sample> input_samples;
  const bool input_ok = test_input(ctx, input_samples);
  release_bus(ctx);

  std::vector<Sample> iio_samples;
  std::vector<Sample> custom_samples;
  double iio_scale = 0;
  int custom_lsb = 0;
  const bool iio_ok = test_iio(ctx, iio_samples, iio_scale);
  release_bus(ctx);
  const bool custom_ok = test_custom(ctx, custom_samples, custom_lsb);

  if (!input_ok || !iio_ok || !custom_ok) {
    std::println("\nresult: FAIL");
    std::println("image adxl34x samples: {}", input_ok ? "yes" : "no");
    std::println("IIO samples: {}", iio_ok ? "yes" : "no");
    std::println("custom samples: {}", custom_ok ? "yes" : "no");
    if (ctx.keep_custom) {
      std::println("left loaded: adxl345_custom  /dev/adxl345_custom");
    }
    return 2;
  }
  const int code = compare(summarize(iio_samples), summarize(custom_samples), iio_scale,
                           summarize(input_samples));
  std::println("left loaded: adxl345_custom  /dev/adxl345_custom");
  std::println("unload with: rmmod adxl345_custom");
  return code;
}

}  // namespace

int main(int argc, char** argv) {
  Context ctx;
  int code = 2;
  try {
    code = compare_main(ctx, argc, argv);
  } catch (const std::exception& ex) {
    std::println(std::cerr, "error: {}", ex.what());
    code = 2;
  }
  try {
    cleanup(ctx);
  } catch (const std::exception& ex) {
    std::println(std::cerr, "cleanup error: {}", ex.what());
  }
  return code;
}
