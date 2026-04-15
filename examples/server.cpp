#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cstp/protocol.hpp"
#include "cstp/socket_io.hpp"

namespace {

class FdGuard {
 public:
  explicit FdGuard(int fd) : fd_(fd) {}
  ~FdGuard() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;

  int Get() const { return fd_; }

 private:
  int fd_;
};

enum class GpioMode {
  kMock,
  kSysfs,
};

enum class PwmBackend {
  kAuto,
  kPigpio,
  kSysfsSoft,
};

struct CommandExecutionResult {
  std::uint16_t status_code = 500;
  std::string message = "internal_error";
  std::string result_json = "{}";
};

struct EndpointDefinition {
  std::string kind = "digital_out";
  int pin = 17;
  bool active_low = false;
  int min_pulse_us = 500;
  int max_pulse_us = 2500;
};

using EndpointRegistry = std::unordered_map<std::string, EndpointDefinition>;

bool IsClientDisconnect(const std::exception& e) {
  return std::string(e.what()) == "peer closed socket while receiving data";
}

GpioMode ResolveGpioMode() {
  const char* env = std::getenv("CSTP_GPIO_MODE");
  if (env != nullptr) {
    const std::string value(env);
    if (value == "mock") {
      return GpioMode::kMock;
    }
    if (value == "sysfs") {
      return GpioMode::kSysfs;
    }
  }
#if defined(__linux__)
  return GpioMode::kSysfs;
#else
  return GpioMode::kMock;
#endif
}

int ResolveDefaultLedPin() {
  const char* env = std::getenv("CSTP_LED_PIN");
  if (env == nullptr) {
    return 17;
  }
  try {
    return std::stoi(env);
  } catch (...) {
    return 17;
  }
}

std::string GpioModeName(GpioMode mode) {
  return mode == GpioMode::kSysfs ? "sysfs" : "mock";
}

PwmBackend ResolvePwmBackend() {
  const char* env = std::getenv("CSTP_PWM_BACKEND");
  if (env == nullptr) {
    return PwmBackend::kAuto;
  }
  const std::string value(env);
  if (value == "auto") {
    return PwmBackend::kAuto;
  }
  if (value == "pigpio") {
    return PwmBackend::kPigpio;
  }
  if (value == "sysfs_soft") {
    return PwmBackend::kSysfsSoft;
  }
  return PwmBackend::kAuto;
}

std::string PwmBackendName(PwmBackend backend) {
  switch (backend) {
    case PwmBackend::kAuto:
      return "auto";
    case PwmBackend::kPigpio:
      return "pigpio";
    case PwmBackend::kSysfsSoft:
      return "sysfs_soft";
  }
  return "auto";
}

std::optional<std::size_t> FindKeyStart(const std::string& json, const std::string& key) {
  const std::string quoted = "\"" + key + "\"";
  const std::size_t quoted_pos = json.find(quoted);
  if (quoted_pos != std::string::npos) {
    return quoted_pos + quoted.size();
  }

  const std::size_t bare_pos = json.find(key);
  if (bare_pos != std::string::npos) {
    return bare_pos + key.size();
  }
  return std::nullopt;
}

std::optional<std::string> ExtractRawJsonValue(const std::string& json, const std::string& key) {
  const auto key_end = FindKeyStart(json, key);
  if (!key_end.has_value()) {
    return std::nullopt;
  }

  const std::size_t colon = json.find(':', *key_end);
  if (colon == std::string::npos) {
    return std::nullopt;
  }

  std::size_t start = colon + 1;
  while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) {
    ++start;
  }
  if (start >= json.size()) {
    return std::nullopt;
  }

  std::size_t end = start;
  if (json[start] == '"') {
    ++start;
    end = start;
    while (end < json.size() && json[end] != '"') {
      ++end;
    }
    if (end >= json.size()) {
      return std::nullopt;
    }
    return json.substr(start, end - start);
  }

  while (end < json.size() && json[end] != ',' && json[end] != '}' &&
         !std::isspace(static_cast<unsigned char>(json[end]))) {
    ++end;
  }
  return json.substr(start, end - start);
}

std::optional<int> ExtractIntArg(const std::string& json, const std::string& key) {
  const auto raw = ExtractRawJsonValue(json, key);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  try {
    return std::stoi(*raw);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<bool> ExtractBoolArg(const std::string& json, const std::string& key) {
  const auto raw = ExtractRawJsonValue(json, key);
  if (!raw.has_value()) {
    return std::nullopt;
  }

  std::string token;
  token.reserve(raw->size());
  for (char ch : *raw) {
    token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  if (token == "1" || token == "true" || token == "on") {
    return true;
  }
  if (token == "0" || token == "false" || token == "off") {
    return false;
  }
  return std::nullopt;
}

std::optional<std::string> ExtractStringArg(const std::string& json, const std::string& key) {
  return ExtractRawJsonValue(json, key);
}

bool WriteFile(const std::string& path, std::string_view content, std::string* error) {
  std::ofstream out(path);
  if (!out) {
    *error = "open failed: " + path;
    return false;
  }
  out << content;
  if (!out.good()) {
    *error = "write failed: " + path;
    return false;
  }
  return true;
}

bool ExistsFile(const std::string& path) {
  std::ifstream in(path);
  return in.good();
}

class SoftPwmManager {
 public:
  SoftPwmManager() = default;
  ~SoftPwmManager() { Shutdown(); }

  SoftPwmManager(const SoftPwmManager&) = delete;
  SoftPwmManager& operator=(const SoftPwmManager&) = delete;

  bool SetPulseUs(int pin, int pulse_us, std::string* error) {
    if (pin < 0) {
      *error = "invalid pin";
      return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(pin);
    if (it == workers_.end()) {
      auto worker = std::make_unique<Worker>();
      worker->pin = pin;
      worker->pulse_us.store(pulse_us, std::memory_order_relaxed);

      if (!PrepareWorker(worker.get(), error)) {
        return false;
      }

      worker->thread = std::thread([this, raw_worker = worker.get()]() {
        WorkerLoop(raw_worker);
      });
      workers_[pin] = std::move(worker);
      return true;
    }

    it->second->pulse_us.store(pulse_us, std::memory_order_relaxed);
    return true;
  }

  void Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [pin, worker] : workers_) {
      (void)pin;
      worker->stop.store(true, std::memory_order_relaxed);
    }
    for (auto& [pin, worker] : workers_) {
      (void)pin;
      if (worker->thread.joinable()) {
        worker->thread.join();
      }
      if (worker->value_fd >= 0) {
        close(worker->value_fd);
        worker->value_fd = -1;
      }
    }
    workers_.clear();
  }

 private:
  struct Worker {
    int pin = -1;
    int value_fd = -1;
    std::string value_path;
    std::atomic<int> pulse_us{0};
    std::atomic<bool> stop{false};
    std::thread thread;
  };

  static bool EnsureOutputPinReady(int pin, std::string* value_path, std::string* error) {
    const std::string gpio_base = "/sys/class/gpio/gpio" + std::to_string(pin);
    const std::string out_value_path = gpio_base + "/value";
    const std::string direction_path = gpio_base + "/direction";

    if (!ExistsFile(out_value_path)) {
      if (!WriteFile("/sys/class/gpio/export", std::to_string(pin), error) &&
          !ExistsFile(out_value_path)) {
        return false;
      }
    }

    if (!WriteFile(direction_path, "out", error)) {
      return false;
    }

    *value_path = out_value_path;
    return true;
  }

  static bool WriteToFd(int fd, char value, std::string* error) {
    if (lseek(fd, 0, SEEK_SET) < 0) {
      if (error != nullptr) {
        *error = "lseek failed: " + std::string(std::strerror(errno));
      }
      return false;
    }
    const ssize_t written = write(fd, &value, 1);
    if (written != 1) {
      if (error != nullptr) {
        *error = "write failed: " + std::string(std::strerror(errno));
      }
      return false;
    }
    return true;
  }

  static bool PrepareWorker(Worker* worker, std::string* error) {
    if (!EnsureOutputPinReady(worker->pin, &worker->value_path, error)) {
      return false;
    }

    worker->value_fd = open(worker->value_path.c_str(), O_WRONLY);
    if (worker->value_fd < 0) {
      *error = "open failed: " + worker->value_path + ": " + std::string(std::strerror(errno));
      return false;
    }
    if (!WriteToFd(worker->value_fd, '0', error)) {
      close(worker->value_fd);
      worker->value_fd = -1;
      return false;
    }
    return true;
  }

  static void SleepMicros(int micros) {
    if (micros > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(micros));
    }
  }

  static void WorkerLoop(Worker* worker) {
    constexpr int kPeriodUs = 20000;
    constexpr int kMinPulseUs = 500;
    constexpr int kMaxPulseUs = 2500;
    bool logged_io_error = false;

    while (!worker->stop.load(std::memory_order_relaxed)) {
      int pulse = worker->pulse_us.load(std::memory_order_relaxed);
      if (pulse < 0) {
        pulse = 0;
      }
      if (pulse > 0) {
        pulse = std::clamp(pulse, kMinPulseUs, kMaxPulseUs);
      }

      if (pulse == 0) {
        std::string write_error;
        if (!WriteToFd(worker->value_fd, '0', &write_error) && !logged_io_error) {
          std::cerr << "soft PWM pin=" << worker->pin << " write failed: " << write_error << "\n";
          logged_io_error = true;
        }
        SleepMicros(kPeriodUs);
        continue;
      }

      std::string write_error;
      if (!WriteToFd(worker->value_fd, '1', &write_error) && !logged_io_error) {
        std::cerr << "soft PWM pin=" << worker->pin << " write failed: " << write_error << "\n";
        logged_io_error = true;
      }
      SleepMicros(pulse);
      if (!WriteToFd(worker->value_fd, '0', &write_error) && !logged_io_error) {
        std::cerr << "soft PWM pin=" << worker->pin << " write failed: " << write_error << "\n";
        logged_io_error = true;
      }
      SleepMicros(kPeriodUs - pulse);
    }

    std::string write_error;
    (void)WriteToFd(worker->value_fd, '0', &write_error);
  }

  std::mutex mutex_;
  std::unordered_map<int, std::unique_ptr<Worker>> workers_;
};

bool SetLedSysfs(int pin, bool value, std::string* error) {
  if (pin < 0) {
    *error = "invalid pin";
    return false;
  }

  const std::string gpio_base = "/sys/class/gpio/gpio" + std::to_string(pin);
  const std::string value_path = gpio_base + "/value";
  const std::string direction_path = gpio_base + "/direction";

  if (!ExistsFile(value_path)) {
    if (!WriteFile("/sys/class/gpio/export", std::to_string(pin), error) && !ExistsFile(value_path)) {
      return false;
    }
  }

  if (!WriteFile(direction_path, "out", error)) {
    return false;
  }
  if (!WriteFile(value_path, value ? "1" : "0", error)) {
    return false;
  }
  return true;
}

bool SetServoPulsePigpio(int pin, int pulse_us, std::string* error) {
  if (pin < 0) {
    *error = "invalid pin";
    return false;
  }
  if (pulse_us != 0 && (pulse_us < 500 || pulse_us > 2500)) {
    *error = "pulse_us must be 500..2500 (or 0 to stop)";
    return false;
  }

  std::array<char, 128> cmd{};
  const int n = std::snprintf(cmd.data(), cmd.size(), "pigs s %d %d 2>&1", pin, pulse_us);
  if (n <= 0 || static_cast<std::size_t>(n) >= cmd.size()) {
    *error = "failed to build pigpio command";
    return false;
  }

  std::array<char, 256> output{};
  std::string captured;
  FILE* pipe = popen(cmd.data(), "r");
  if (pipe == nullptr) {
    *error = "popen failed for pigs command";
    return false;
  }
  while (std::fgets(output.data(), static_cast<int>(output.size()), pipe) != nullptr) {
    captured += output.data();
  }
  const int rc = pclose(pipe);
  if (rc != 0) {
    while (!captured.empty() && captured.back() == '\n') {
      captured.pop_back();
    }
    *error = "pigs command failed (rc=" + std::to_string(rc) + "): " +
             (captured.empty() ? "is pigpiod running?" : captured);
    return false;
  }
  std::cout << "pigpio: set servo pin=" << pin << " pulse_us=" << pulse_us << "\n";
  return true;
}

bool ApplyDigitalOutput(int pin, bool logical_value, bool active_low, GpioMode gpio_mode,
                        std::string* error) {
  const bool gpio_value = active_low ? !logical_value : logical_value;
  if (gpio_mode == GpioMode::kMock) {
    std::cout << "MOCK GPIO pin=" << pin << " logical=" << (logical_value ? 1 : 0)
              << " gpio=" << (gpio_value ? 1 : 0) << "\n";
    return true;
  }
  return SetLedSysfs(pin, gpio_value, error);
}

bool ApplyPwmOutput(int pin, int pulse_us, int min_pulse_us, int max_pulse_us, GpioMode gpio_mode,
                    PwmBackend pwm_backend, SoftPwmManager* soft_pwm, std::string* error) {
  if (pulse_us != 0 && (pulse_us < min_pulse_us || pulse_us > max_pulse_us)) {
    *error = "pulse_us must be within endpoint range";
    return false;
  }
  if (gpio_mode == GpioMode::kMock) {
    std::cout << "MOCK PWM pin=" << pin << " pulse_us=" << pulse_us << "\n";
    return true;
  }

  if (pwm_backend == PwmBackend::kPigpio) {
    return SetServoPulsePigpio(pin, pulse_us, error);
  }
  if (pwm_backend == PwmBackend::kSysfsSoft) {
    if (soft_pwm == nullptr) {
      *error = "soft PWM manager unavailable";
      return false;
    }
    return soft_pwm->SetPulseUs(pin, pulse_us, error);
  }

  std::string pigpio_error;
  if (SetServoPulsePigpio(pin, pulse_us, &pigpio_error)) {
    std::cout << "PWM auto: using pigpio for pin=" << pin << "\n";
    return true;
  }
  std::cerr << "PWM auto: pigpio failed: " << pigpio_error << "\n";

  if (soft_pwm != nullptr) {
    std::string soft_pwm_error;
    if (soft_pwm->SetPulseUs(pin, pulse_us, &soft_pwm_error)) {
      std::cout << "PWM auto: falling back to sysfs_soft for pin=" << pin << "\n";
      return true;
    }
    *error = "pigpio failed: " + pigpio_error + "; sysfs_soft failed: " + soft_pwm_error;
    return false;
  }

  *error = "pigpio failed: " + pigpio_error;
  return false;
}

class ServoSweepManager {
 public:
  ServoSweepManager() = default;
  ~ServoSweepManager() { StopAll(); }

  ServoSweepManager(const ServoSweepManager&) = delete;
  ServoSweepManager& operator=(const ServoSweepManager&) = delete;

  bool Start(int pin, int min_us, int max_us, int step_us, int step_delay_ms,
             GpioMode gpio_mode, PwmBackend pwm_backend, SoftPwmManager* soft_pwm,
             std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(pin);
    if (it != workers_.end()) {
      it->second->stop.store(true, std::memory_order_relaxed);
      if (it->second->thread.joinable()) {
        it->second->thread.join();
      }
      workers_.erase(it);
    }

    auto worker = std::make_unique<Worker>();
    worker->pin = pin;
    worker->min_us = min_us;
    worker->max_us = max_us;
    worker->step_us = step_us;
    worker->step_delay_ms = step_delay_ms;
    worker->gpio_mode = gpio_mode;
    worker->pwm_backend = pwm_backend;
    worker->soft_pwm = soft_pwm;

    worker->thread = std::thread([raw = worker.get()]() { SweepLoop(raw); });
    workers_[pin] = std::move(worker);
    return true;
  }

  bool Stop(int pin, GpioMode gpio_mode, PwmBackend pwm_backend, SoftPwmManager* soft_pwm,
            std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(pin);
    if (it == workers_.end()) {
      *error = "no sweep running on pin " + std::to_string(pin);
      return false;
    }
    it->second->stop.store(true, std::memory_order_relaxed);
    if (it->second->thread.joinable()) {
      it->second->thread.join();
    }
    workers_.erase(it);

    std::string pwm_error;
    (void)ApplyPwmOutput(pin, 0, 0, 2500, gpio_mode, pwm_backend, soft_pwm, &pwm_error);
    return true;
  }

  void StopAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [pin, worker] : workers_) {
      (void)pin;
      worker->stop.store(true, std::memory_order_relaxed);
    }
    for (auto& [pin, worker] : workers_) {
      (void)pin;
      if (worker->thread.joinable()) {
        worker->thread.join();
      }
    }
    workers_.clear();
  }

 private:
  struct Worker {
    int pin = -1;
    int min_us = 500;
    int max_us = 2500;
    int step_us = 50;
    int step_delay_ms = 20;
    GpioMode gpio_mode = GpioMode::kMock;
    PwmBackend pwm_backend = PwmBackend::kAuto;
    SoftPwmManager* soft_pwm = nullptr;
    std::atomic<bool> stop{false};
    std::thread thread;
  };

  static void SweepLoop(Worker* w) {
    int pulse = w->min_us;
    int direction = 1;

    while (!w->stop.load(std::memory_order_relaxed)) {
      std::string error;
      if (!ApplyPwmOutput(w->pin, pulse, w->min_us, w->max_us, w->gpio_mode,
                          w->pwm_backend, w->soft_pwm, &error)) {
        std::cerr << "sweep pin=" << w->pin << " failed: " << error << "\n";
        return;
      }

      pulse += direction * w->step_us;
      if (pulse >= w->max_us) {
        pulse = w->max_us;
        direction = -1;
      } else if (pulse <= w->min_us) {
        pulse = w->min_us;
        direction = 1;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(w->step_delay_ms));
    }
  }

  std::mutex mutex_;
  std::unordered_map<int, std::unique_ptr<Worker>> workers_;
};

bool IsSupportedEndpointKind(const std::string& kind) {
  return kind == "digital_out" || kind == "pwm_out";
}

std::string BuildEndpointRegistryJson(const EndpointRegistry& endpoint_registry) {
  std::string out = "{\"endpoints\":[";
  bool first = true;
  for (const auto& [name, endpoint] : endpoint_registry) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += "{\"name\":\"" + name + "\",\"kind\":\"" + endpoint.kind + "\",\"pin\":" +
           std::to_string(endpoint.pin) + ",\"active_low\":" +
           std::to_string(endpoint.active_low ? 1 : 0) + ",\"min_pulse_us\":" +
           std::to_string(endpoint.min_pulse_us) + ",\"max_pulse_us\":" +
           std::to_string(endpoint.max_pulse_us) + "}";
  }
  out += "]}";
  return out;
}

CommandExecutionResult ExecuteCommand(const cstp::CommandRequestPayload& request,
                                      const std::string& local_device_id, GpioMode gpio_mode,
                                      PwmBackend pwm_backend, int default_led_pin,
                                      EndpointRegistry* endpoint_registry,
                                      SoftPwmManager* soft_pwm,
                                      ServoSweepManager* servo_sweep) {
  CommandExecutionResult out;

  if (!request.target_device_id.empty() && request.target_device_id != local_device_id) {
    out.status_code = 404;
    out.message = "target_device_id does not match this receiver";
    return out;
  }

  if (request.command_name == "endpoint.list") {
    out.status_code = 200;
    out.message = "ok";
    out.result_json = BuildEndpointRegistryJson(*endpoint_registry);
    return out;
  }

  if (request.command_name == "endpoint.unregister") {
    const auto name_arg = ExtractStringArg(request.args_json, "name");
    if (!name_arg.has_value() || name_arg->empty()) {
      out.status_code = 400;
      out.message = "endpoint.unregister requires args_json.name";
      return out;
    }
    const std::size_t erased = endpoint_registry->erase(*name_arg);
    if (erased == 0) {
      out.status_code = 404;
      out.message = "unknown endpoint name: " + *name_arg;
      return out;
    }
    out.status_code = 200;
    out.message = "ok";
    out.result_json = "{\"name\":\"" + *name_arg + "\",\"removed\":1}";
    return out;
  }

  if (request.command_name == "endpoint.register") {
    const auto name_arg = ExtractStringArg(request.args_json, "name");
    if (!name_arg.has_value() || name_arg->empty()) {
      out.status_code = 400;
      out.message = "endpoint.register requires args_json.name";
      return out;
    }

    std::string kind = "digital_out";
    const auto kind_arg = ExtractStringArg(request.args_json, "kind");
    if (kind_arg.has_value() && !kind_arg->empty()) {
      kind = *kind_arg;
    }
    if (!IsSupportedEndpointKind(kind)) {
      out.status_code = 400;
      out.message = "unsupported endpoint kind: " + kind;
      return out;
    }

    const auto pin_arg = ExtractIntArg(request.args_json, "pin");
    const int pin = pin_arg.has_value() ? *pin_arg : default_led_pin;
    if (pin < 0) {
      out.status_code = 400;
      out.message = "endpoint pin must be >= 0";
      return out;
    }

    const auto active_low_arg = ExtractBoolArg(request.args_json, "active_low");
    const auto min_pulse_arg = ExtractIntArg(request.args_json, "min_pulse_us");
    const auto max_pulse_arg = ExtractIntArg(request.args_json, "max_pulse_us");
    const int min_pulse_us = min_pulse_arg.has_value() ? *min_pulse_arg : 500;
    const int max_pulse_us = max_pulse_arg.has_value() ? *max_pulse_arg : 2500;
    if (min_pulse_us <= 0 || max_pulse_us <= 0 || min_pulse_us > max_pulse_us) {
      out.status_code = 400;
      out.message = "invalid min_pulse_us/max_pulse_us";
      return out;
    }

    (*endpoint_registry)[*name_arg] = EndpointDefinition{
        .kind = kind,
        .pin = pin,
        .active_low = active_low_arg.has_value() ? *active_low_arg : false,
        .min_pulse_us = min_pulse_us,
        .max_pulse_us = max_pulse_us,
    };

    out.status_code = 200;
    out.message = "ok";
    out.result_json =
        "{\"name\":\"" + *name_arg + "\",\"kind\":\"" + kind + "\",\"pin\":" +
        std::to_string(pin) + ",\"active_low\":" +
        std::to_string(active_low_arg.has_value() && *active_low_arg ? 1 : 0) +
        ",\"min_pulse_us\":" + std::to_string(min_pulse_us) + ",\"max_pulse_us\":" +
        std::to_string(max_pulse_us) + "}";
    return out;
  }

  if (request.command_name == "endpoint.write") {
    const auto name_arg = ExtractStringArg(request.args_json, "name");
    if (!name_arg.has_value() || name_arg->empty()) {
      out.status_code = 400;
      out.message = "endpoint.write requires args_json.name";
      return out;
    }
    const auto it = endpoint_registry->find(*name_arg);
    if (it == endpoint_registry->end()) {
      out.status_code = 404;
      out.message = "unknown endpoint name: " + *name_arg;
      return out;
    }
    const EndpointDefinition endpoint = it->second;

    if (endpoint.kind == "digital_out") {
      const auto value_arg = ExtractBoolArg(request.args_json, "value");
      if (!value_arg.has_value()) {
        out.status_code = 400;
        out.message = "endpoint.write(digital_out) requires args_json.value";
        return out;
      }
      std::string error;
      if (!ApplyDigitalOutput(endpoint.pin, *value_arg, endpoint.active_low, gpio_mode, &error)) {
        out.status_code = 500;
        out.message = "digital_write_failed: " + error;
        return out;
      }
      out.status_code = 200;
      out.message = "ok";
      out.result_json = "{\"name\":\"" + *name_arg + "\",\"kind\":\"digital_out\",\"pin\":" +
                        std::to_string(endpoint.pin) + ",\"value\":" +
                        std::to_string(*value_arg ? 1 : 0) + "}";
      return out;
    }

    if (endpoint.kind == "pwm_out") {
      const auto pulse_arg = ExtractIntArg(request.args_json, "pulse_us");
      if (!pulse_arg.has_value()) {
        out.status_code = 400;
        out.message = "endpoint.write(pwm_out) requires args_json.pulse_us";
        return out;
      }
      std::string error;
      if (!ApplyPwmOutput(endpoint.pin, *pulse_arg, endpoint.min_pulse_us, endpoint.max_pulse_us,
                          gpio_mode, pwm_backend, soft_pwm, &error)) {
        out.status_code = 500;
        out.message = "pwm_write_failed: " + error;
        return out;
      }
      out.status_code = 200;
      out.message = "ok";
      out.result_json = "{\"name\":\"" + *name_arg + "\",\"kind\":\"pwm_out\",\"pin\":" +
                        std::to_string(endpoint.pin) + ",\"pulse_us\":" +
                        std::to_string(*pulse_arg) + "}";
      return out;
    }

    out.status_code = 400;
    out.message = "endpoint.write unsupported kind: " + endpoint.kind;
    return out;
  }

  if (request.command_name == "pin.write") {
    const auto pin_arg = ExtractIntArg(request.args_json, "pin");
    const auto value_arg = ExtractBoolArg(request.args_json, "value");
    if (!pin_arg.has_value() || !value_arg.has_value()) {
      out.status_code = 400;
      out.message = "pin.write requires args_json.pin and args_json.value";
      return out;
    }
    const auto active_low_arg = ExtractBoolArg(request.args_json, "active_low");
    std::string error;
    if (!ApplyDigitalOutput(*pin_arg, *value_arg,
                            active_low_arg.has_value() ? *active_low_arg : false, gpio_mode,
                            &error)) {
      out.status_code = 500;
      out.message = "digital_write_failed: " + error;
      return out;
    }
    out.status_code = 200;
    out.message = "ok";
    out.result_json = "{\"pin\":" + std::to_string(*pin_arg) + ",\"value\":" +
                      std::to_string(*value_arg ? 1 : 0) + "}";
    return out;
  }

  if (request.command_name == "pin.pwm") {
    const auto pin_arg = ExtractIntArg(request.args_json, "pin");
    if (!pin_arg.has_value()) {
      out.status_code = 400;
      out.message = "pin.pwm requires args_json.pin";
      return out;
    }

    const auto pulse_arg = ExtractIntArg(request.args_json, "pulse_us");
    if (!pulse_arg.has_value()) {
      out.status_code = 400;
      out.message = "pin.pwm requires args_json.pulse_us";
      return out;
    }

    std::string error;
    if (!ApplyPwmOutput(*pin_arg, *pulse_arg, 500, 2500, gpio_mode, pwm_backend, soft_pwm,
                        &error)) {
      out.status_code = 500;
      out.message = "pwm_write_failed: " + error;
      return out;
    }

    out.status_code = 200;
    out.message = "ok";
    out.result_json = "{\"pin\":" + std::to_string(*pin_arg) +
                      ",\"pulse_us\":" + std::to_string(*pulse_arg) + "}";
    return out;
  }

  if (request.command_name == "pin.servo_sweep") {
    const auto pin_arg = ExtractIntArg(request.args_json, "pin");
    if (!pin_arg.has_value()) {
      out.status_code = 400;
      out.message = "pin.servo_sweep requires args_json.pin";
      return out;
    }

    const auto min_arg = ExtractIntArg(request.args_json, "min_us");
    const auto max_arg = ExtractIntArg(request.args_json, "max_us");
    const auto step_arg = ExtractIntArg(request.args_json, "step_us");
    const auto delay_arg = ExtractIntArg(request.args_json, "step_delay_ms");
    const int min_us = min_arg.has_value() ? *min_arg : 500;
    const int max_us = max_arg.has_value() ? *max_arg : 2500;
    const int step_us = step_arg.has_value() ? *step_arg : 50;
    const int step_delay_ms = delay_arg.has_value() ? *delay_arg : 20;

    std::string error;
    if (!servo_sweep->Start(*pin_arg, min_us, max_us, step_us, step_delay_ms,
                            gpio_mode, pwm_backend, soft_pwm, &error)) {
      out.status_code = 500;
      out.message = "servo_sweep_failed: " + error;
      return out;
    }

    out.status_code = 200;
    out.message = "ok";
    out.result_json = "{\"pin\":" + std::to_string(*pin_arg) +
                      ",\"min_us\":" + std::to_string(min_us) +
                      ",\"max_us\":" + std::to_string(max_us) +
                      ",\"step_us\":" + std::to_string(step_us) +
                      ",\"step_delay_ms\":" + std::to_string(step_delay_ms) + "}";
    return out;
  }

  if (request.command_name == "pin.servo_stop") {
    const auto pin_arg = ExtractIntArg(request.args_json, "pin");
    if (!pin_arg.has_value()) {
      out.status_code = 400;
      out.message = "pin.servo_stop requires args_json.pin";
      return out;
    }

    std::string error;
    if (!servo_sweep->Stop(*pin_arg, gpio_mode, pwm_backend, soft_pwm, &error)) {
      out.status_code = 404;
      out.message = error;
      return out;
    }

    out.status_code = 200;
    out.message = "ok";
    out.result_json = "{\"pin\":" + std::to_string(*pin_arg) + ",\"stopped\":1}";
    return out;
  }

  out.status_code = 400;
  out.message = "unsupported command_name";
  return out;
}

void LogFrameSummary(const cstp::Frame& frame) {
  std::cout << "received " << cstp::ToString(frame.header.type) << " msg_id="
            << frame.header.message_id << " payload_bytes=" << frame.payload.size() << "\n";
}

void HandleClient(int client_fd, GpioMode gpio_mode, int default_led_pin,
                  PwmBackend pwm_backend, EndpointRegistry* endpoint_registry,
                  SoftPwmManager* soft_pwm, ServoSweepManager* servo_sweep) {
  const cstp::Frame hello_frame = cstp::ReceiveFrame(client_fd);
  if (hello_frame.header.type != cstp::MessageType::kHello) {
    throw cstp::ProtocolError("expected HELLO as first message");
  }

  const cstp::HelloPayload hello = cstp::DecodeHelloPayload(hello_frame.payload);
  std::cout << "HELLO from device_id=" << hello.device_id
            << " client_version=" << hello.client_version_name << "\n";

  cstp::HelloAckPayload ack;
  ack.accepted = true;
  ack.session_id = 1;
  ack.server_time_ms = cstp::UnixMillisNow();
  ack.accepted_keepalive_sec = hello.requested_keepalive_sec;
  ack.accepted_max_frame_bytes =
      static_cast<std::uint32_t>(std::min<std::size_t>(hello.client_max_frame_bytes,
                                                       cstp::kDefaultMaxFrameBytes));
  ack.reason = "ok";

  cstp::Frame hello_ack_frame;
  hello_ack_frame.header.type = cstp::MessageType::kHelloAck;
  hello_ack_frame.header.flags = cstp::FrameFlags::kIsAck;
  hello_ack_frame.header.message_id = hello_frame.header.message_id;
  hello_ack_frame.header.stream_id = cstp::StreamId::kControl;
  hello_ack_frame.header.timestamp_ms = cstp::UnixMillisNow();
  hello_ack_frame.payload = cstp::EncodeHelloAckPayload(ack);

  cstp::SendFrame(client_fd, hello_ack_frame);
  std::cout << "HELLO_ACK sent\n";

  for (;;) {
    const cstp::Frame frame = cstp::ReceiveFrame(client_fd);
    LogFrameSummary(frame);

    switch (frame.header.type) {
      case cstp::MessageType::kDataBatch: {
        const cstp::DataBatchPayload batch = cstp::DecodeDataBatchPayload(frame.payload);
        std::cout << "DATA_BATCH batch_id=" << batch.batch_id
                  << " sensors=" << batch.sensors.size()
                  << " device_id=" << batch.device_id << "\n";

        cstp::DataAckPayload ack;
        ack.acked_message_id = frame.header.message_id;
        ack.batch_id = batch.batch_id;
        ack.status = cstp::DataAckStatus::kAccepted;
        ack.received_at_ms = cstp::UnixMillisNow();
        ack.note = "accepted";

        cstp::Frame ack_frame;
        ack_frame.header.type = cstp::MessageType::kDataAck;
        ack_frame.header.flags = cstp::FrameFlags::kIsAck;
        ack_frame.header.message_id = frame.header.message_id;
        ack_frame.header.stream_id = cstp::StreamId::kData;
        ack_frame.header.timestamp_ms = cstp::UnixMillisNow();
        ack_frame.payload = cstp::EncodeDataAckPayload(ack);
        cstp::SendFrame(client_fd, ack_frame);

        std::cout << "DATA_ACK sent for msg_id=" << frame.header.message_id
                  << " batch_id=" << batch.batch_id << "\n";
        break;
      }
      case cstp::MessageType::kHeartbeat: {
        const cstp::HeartbeatPayload heartbeat = cstp::DecodeHeartbeatPayload(frame.payload);
        std::cout << "HEARTBEAT uptime_ms=" << heartbeat.uptime_ms
                  << " pending_batches=" << heartbeat.pending_batches
                  << " last_batch_id=" << heartbeat.last_batch_id << "\n";
        break;
      }
      case cstp::MessageType::kCmdReq: {
        const cstp::CommandRequestPayload request = cstp::DecodeCommandRequestPayload(frame.payload);
        std::cout << "CMD_REQ command_id=" << request.command_id
                  << " command_name=" << request.command_name << "\n";

        const CommandExecutionResult exec =
            ExecuteCommand(request, hello.device_id, gpio_mode, pwm_backend, default_led_pin,
                           endpoint_registry, soft_pwm, servo_sweep);

        cstp::CommandResponsePayload response;
        response.command_id = request.command_id;
        response.status_code = exec.status_code;
        response.message = exec.message;
        response.result_json = exec.result_json;

        cstp::Frame response_frame;
        response_frame.header.type = cstp::MessageType::kCmdResp;
        response_frame.header.flags = cstp::FrameFlags::kIsAck;
        response_frame.header.message_id = frame.header.message_id;
        response_frame.header.stream_id = cstp::StreamId::kControl;
        response_frame.header.timestamp_ms = cstp::UnixMillisNow();
        response_frame.payload = cstp::EncodeCommandResponsePayload(response);
        cstp::SendFrame(client_fd, response_frame);

        std::cout << "CMD_RESP sent command_id=" << response.command_id
                  << " status_code=" << response.status_code << "\n";
        break;
      }
      case cstp::MessageType::kCmdResp: {
        const cstp::CommandResponsePayload response =
            cstp::DecodeCommandResponsePayload(frame.payload);
        std::cout << "CMD_RESP command_id=" << response.command_id
                  << " status_code=" << response.status_code << "\n";
        break;
      }
      case cstp::MessageType::kError: {
        const cstp::ErrorPayload error = cstp::DecodeErrorPayload(frame.payload);
        std::cout << "ERROR code=" << static_cast<std::uint16_t>(error.code)
                  << " related_msg_id=" << error.related_message_id
                  << " message=" << error.message << "\n";
        break;
      }
      default:
        break;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int port = (argc > 1) ? std::atoi(argv[1]) : 18830;
  const GpioMode gpio_mode = ResolveGpioMode();
  const PwmBackend pwm_backend = ResolvePwmBackend();
  const int default_led_pin = ResolveDefaultLedPin();
  SoftPwmManager soft_pwm;
  ServoSweepManager servo_sweep;
  EndpointRegistry endpoint_registry;
  endpoint_registry["default"] = EndpointDefinition{
      .kind = "digital_out",
      .pin = default_led_pin,
      .active_low = false,
      .min_pulse_us = 500,
      .max_pulse_us = 2500,
  };
  endpoint_registry["servo_default"] = EndpointDefinition{
      .kind = "pwm_out",
      .pin = default_led_pin,
      .active_low = false,
      .min_pulse_us = 500,
      .max_pulse_us = 2500,
  };

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "socket() failed\n";
    return 1;
  }
  FdGuard server_guard(server_fd);

  int yes = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));

  if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "bind() failed\n";
    return 1;
  }
  if (listen(server_fd, 8) != 0) {
    std::cerr << "listen() failed\n";
    return 1;
  }

  std::cout << "CSTP server listening on port " << port << "\n";
  std::cout << "GPIO mode=" << GpioModeName(gpio_mode)
            << " default_led_pin=" << default_led_pin << "\n";
  std::cout << "PWM backend=" << PwmBackendName(pwm_backend) << "\n";
  std::cout << "registered endpoints: " << BuildEndpointRegistryJson(endpoint_registry) << "\n";

  for (;;) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    const int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
    if (client_fd < 0) {
      std::cerr << "accept() failed\n";
      continue;
    }
    FdGuard client_guard(client_fd);

    try {
      std::cout << "client connected\n";
      HandleClient(client_fd, gpio_mode, default_led_pin, pwm_backend, &endpoint_registry,
                   &soft_pwm, &servo_sweep);
    } catch (const std::exception& e) {
      if (IsClientDisconnect(e)) {
        std::cout << "client disconnected\n";
      } else {
        std::cerr << "protocol error: " << e.what() << "\n";
      }
    }
  }

  return 0;
}
