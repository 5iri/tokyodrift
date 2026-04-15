#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <netdb.h>
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

 private:
  int fd_;
};

int ConnectTcp(const std::string& host, const std::string& port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) {
    return -1;
  }

  int fd = -1;
  for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
    fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
      break;
    }
    close(fd);
    fd = -1;
  }

  freeaddrinfo(result);
  return fd;
}

}  // namespace

std::vector<std::uint8_t> EncodeFloat64LE(double value) {
  std::uint64_t raw = 0;
  static_assert(sizeof(double) == sizeof(std::uint64_t));
  std::memcpy(&raw, &value, sizeof(raw));

  std::vector<std::uint8_t> out(8);
  out[0] = static_cast<std::uint8_t>(raw & 0xFF);
  out[1] = static_cast<std::uint8_t>((raw >> 8) & 0xFF);
  out[2] = static_cast<std::uint8_t>((raw >> 16) & 0xFF);
  out[3] = static_cast<std::uint8_t>((raw >> 24) & 0xFF);
  out[4] = static_cast<std::uint8_t>((raw >> 32) & 0xFF);
  out[5] = static_cast<std::uint8_t>((raw >> 40) & 0xFF);
  out[6] = static_cast<std::uint8_t>((raw >> 48) & 0xFF);
  out[7] = static_cast<std::uint8_t>((raw >> 56) & 0xFF);
  return out;
}

std::vector<std::uint8_t> EncodeBool(bool value) {
  return std::vector<std::uint8_t>{static_cast<std::uint8_t>(value ? 1 : 0)};
}

std::optional<bool> ParseBoolToken(const std::string& token) {
  if (token == "1" || token == "true" || token == "on") {
    return true;
  }
  if (token == "0" || token == "false" || token == "off") {
    return false;
  }
  return std::nullopt;
}

bool IsSupportedCommand(const std::string& name) {
  return name == "led_on" || name == "led_off" || name == "led_set";
}

int main(int argc, char** argv) {
  const std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
  const std::string port = (argc > 2) ? argv[2] : "18830";
  const std::string command_name = (argc > 3) ? argv[3] : "";

  int command_pin = 17;
  if (argc > 4) {
    try {
      command_pin = std::stoi(argv[4]);
    } catch (...) {
      std::cerr << "invalid pin argument\n";
      return 1;
    }
  }

  std::optional<bool> command_value;
  if (!command_name.empty()) {
    if (!IsSupportedCommand(command_name)) {
      std::cerr << "unsupported command: " << command_name << "\n";
      std::cerr << "supported commands: led_on, led_off, led_set\n";
      return 1;
    }

    if (command_name == "led_set") {
      if (argc <= 5) {
        std::cerr << "led_set requires a value argument: 1/0 true/false on/off\n";
        return 1;
      }
      command_value = ParseBoolToken(argv[5]);
      if (!command_value.has_value()) {
        std::cerr << "invalid led_set value: " << argv[5] << "\n";
        return 1;
      }
    }
  }

  const int fd = ConnectTcp(host, port);
  if (fd < 0) {
    std::cerr << "failed to connect to " << host << ":" << port << "\n";
    return 1;
  }
  FdGuard fd_guard(fd);

  cstp::HelloPayload hello;
  hello.device_id = "device-01";
  hello.auth_token = "secret-token";
  hello.requested_keepalive_sec = 30;
  hello.client_max_frame_bytes = static_cast<std::uint32_t>(cstp::kDefaultMaxFrameBytes);
  hello.client_version_name = "cstp-cpp/0.1";

  cstp::Frame frame;
  frame.header.type = cstp::MessageType::kHello;
  frame.header.flags = cstp::FrameFlags::kAckRequired;
  frame.header.message_id = 1;
  frame.header.stream_id = cstp::StreamId::kControl;
  frame.header.timestamp_ms = cstp::UnixMillisNow();
  frame.payload = cstp::EncodeHelloPayload(hello);

  try {
    cstp::SendFrame(fd, frame);
    std::cout << "HELLO sent\n";

    const cstp::Frame response = cstp::ReceiveFrame(fd);
    if (response.header.type != cstp::MessageType::kHelloAck) {
      throw cstp::ProtocolError("expected HELLO_ACK in response");
    }

    const cstp::HelloAckPayload ack = cstp::DecodeHelloAckPayload(response.payload);
    std::cout << "HELLO_ACK accepted=" << (ack.accepted ? "true" : "false")
              << " session_id=" << ack.session_id << " reason=" << ack.reason << "\n";

    if (command_name.empty()) {
      cstp::DataBatchPayload batch;
      batch.batch_id = 1001;
      batch.device_id = hello.device_id;
      batch.batch_time_ms = cstp::UnixMillisNow();

      cstp::SensorSeries temperature;
      temperature.sensor_id = 1;
      temperature.metric_name = "temperature_c";
      temperature.value_type = cstp::ValueType::kFloat64;
      temperature.samples.push_back(
          cstp::Sample{.delta_ms = 0, .value = EncodeFloat64LE(26.35)});
      temperature.samples.push_back(
          cstp::Sample{.delta_ms = 1000, .value = EncodeFloat64LE(26.62)});

      cstp::SensorSeries motion;
      motion.sensor_id = 2;
      motion.metric_name = "motion";
      motion.value_type = cstp::ValueType::kBool;
      motion.samples.push_back(cstp::Sample{.delta_ms = 0, .value = EncodeBool(false)});
      motion.samples.push_back(cstp::Sample{.delta_ms = 1000, .value = EncodeBool(true)});

      batch.sensors.push_back(temperature);
      batch.sensors.push_back(motion);

      cstp::Frame data_frame;
      data_frame.header.type = cstp::MessageType::kDataBatch;
      data_frame.header.flags = cstp::FrameFlags::kAckRequired;
      data_frame.header.message_id = 2;
      data_frame.header.stream_id = cstp::StreamId::kData;
      data_frame.header.timestamp_ms = cstp::UnixMillisNow();
      data_frame.payload = cstp::EncodeDataBatchPayload(batch);

      cstp::SendFrame(fd, data_frame);
      std::cout << "DATA_BATCH sent msg_id=" << data_frame.header.message_id
                << " batch_id=" << batch.batch_id
                << " sensors=" << batch.sensors.size() << "\n";

      const cstp::Frame data_ack_frame = cstp::ReceiveFrame(fd);
      if (data_ack_frame.header.type != cstp::MessageType::kDataAck) {
        throw cstp::ProtocolError("expected DATA_ACK after DATA_BATCH");
      }

      const cstp::DataAckPayload data_ack = cstp::DecodeDataAckPayload(data_ack_frame.payload);
      std::cout << "DATA_ACK status=" << static_cast<int>(data_ack.status)
                << " acked_msg_id=" << data_ack.acked_message_id
                << " batch_id=" << data_ack.batch_id
                << " note=" << data_ack.note << "\n";
    }

    if (!command_name.empty()) {
      cstp::CommandRequestPayload command_request;
      command_request.command_id = 5001;
      command_request.target_device_id = hello.device_id;
      command_request.target_sensor_id = 65535;
      command_request.command_name = command_name;
      command_request.timeout_ms = 2000;
      command_request.args_json = "{\"pin\":" + std::to_string(command_pin);
      if (command_name == "led_set") {
        command_request.args_json +=
            ",\"value\":" + std::to_string(command_value.value() ? 1 : 0);
      }
      command_request.args_json += "}";

      cstp::Frame command_frame;
      command_frame.header.type = cstp::MessageType::kCmdReq;
      command_frame.header.flags = cstp::FrameFlags::kAckRequired;
      command_frame.header.message_id = 3;
      command_frame.header.stream_id = cstp::StreamId::kControl;
      command_frame.header.timestamp_ms = cstp::UnixMillisNow();
      command_frame.payload = cstp::EncodeCommandRequestPayload(command_request);

      cstp::SendFrame(fd, command_frame);
      std::cout << "CMD_REQ sent command_name=" << command_request.command_name
                << " args_json=" << command_request.args_json << "\n";

      const cstp::Frame command_response_frame = cstp::ReceiveFrame(fd);
      if (command_response_frame.header.type != cstp::MessageType::kCmdResp) {
        throw cstp::ProtocolError("expected CMD_RESP after CMD_REQ");
      }

      const cstp::CommandResponsePayload command_response =
          cstp::DecodeCommandResponsePayload(command_response_frame.payload);
      std::cout << "CMD_RESP command_id=" << command_response.command_id
                << " status_code=" << command_response.status_code
                << " message=" << command_response.message
                << " result_json=" << command_response.result_json << "\n";
    }
  } catch (const std::exception& e) {
    std::cerr << "protocol error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
