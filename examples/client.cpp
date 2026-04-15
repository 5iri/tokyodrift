#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
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

int main(int argc, char** argv) {
  enum class ClientMode {
    kDataBatch,
    kCmd,
    kPublish,
    kSubscribe,
  };

  const std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
  const std::string port = (argc > 2) ? argv[2] : "18830";
  const std::string mode_arg = (argc > 3) ? argv[3] : "";

  ClientMode mode = ClientMode::kDataBatch;
  std::string raw_command_name;
  std::string raw_args_json;
  std::string publish_topic;
  std::string publish_text;
  std::uint8_t publish_qos = 1;
  bool publish_retain = false;
  std::string subscribe_filter;
  std::uint8_t subscribe_qos = 1;

  auto ParseU8 = [](const std::string& value, const char* field_name) -> std::uint8_t {
    try {
      const int parsed = std::stoi(value);
      if (parsed < 0 || parsed > 255) {
        throw std::runtime_error("range");
      }
      return static_cast<std::uint8_t>(parsed);
    } catch (...) {
      throw cstp::ProtocolError(std::string("invalid ") + field_name + ": " + value);
    }
  };

  if (!mode_arg.empty()) {
    if (mode_arg == "cmd") {
      if (argc <= 5) {
        std::cerr
            << "usage: ./build/cstp_client <host> <port> cmd <command_name> '<args_json>'\n";
        return 1;
      }
      mode = ClientMode::kCmd;
      raw_command_name = argv[4];
      raw_args_json = argv[5];
    } else if (mode_arg == "pub") {
      if (argc <= 5) {
        std::cerr
            << "usage: ./build/cstp_client <host> <port> pub <topic> '<payload_text>' [qos 0|1] [retain 0|1]\n";
        return 1;
      }
      mode = ClientMode::kPublish;
      publish_topic = argv[4];
      publish_text = argv[5];
      if (argc > 6) {
        publish_qos = ParseU8(argv[6], "qos");
        if (publish_qos > 1) {
          std::cerr << "qos > 1 not supported in this client/server yet\n";
          return 1;
        }
      }
      if (argc > 7) {
        publish_retain = ParseU8(argv[7], "retain") != 0;
      }
    } else if (mode_arg == "sub") {
      if (argc <= 4) {
        std::cerr
            << "usage: ./build/cstp_client <host> <port> sub <topic_filter> [qos 0|1]\n";
        return 1;
      }
      mode = ClientMode::kSubscribe;
      subscribe_filter = argv[4];
      if (argc > 5) {
        subscribe_qos = ParseU8(argv[5], "qos");
        if (subscribe_qos > 1) {
          std::cerr << "qos > 1 not supported in this client/server yet\n";
          return 1;
        }
      }
    } else {
      std::cerr << "unknown mode: " << mode_arg << "\n";
      std::cerr << "supported modes: cmd, pub, sub\n";
      return 1;
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

  std::uint32_t next_message_id = 1;
  cstp::Frame hello_frame;
  hello_frame.header.type = cstp::MessageType::kHello;
  hello_frame.header.flags = cstp::FrameFlags::kAckRequired;
  hello_frame.header.message_id = next_message_id++;
  hello_frame.header.stream_id = cstp::StreamId::kControl;
  hello_frame.header.timestamp_ms = cstp::UnixMillisNow();
  hello_frame.payload = cstp::EncodeHelloPayload(hello);

  try {
    cstp::SendFrame(fd, hello_frame);
    std::cout << "HELLO sent\n";

    const cstp::Frame hello_response = cstp::ReceiveFrame(fd);
    if (hello_response.header.type != cstp::MessageType::kHelloAck) {
      throw cstp::ProtocolError("expected HELLO_ACK in response");
    }

    const cstp::HelloAckPayload ack = cstp::DecodeHelloAckPayload(hello_response.payload);
    std::cout << "HELLO_ACK accepted=" << (ack.accepted ? "true" : "false")
              << " session_id=" << ack.session_id << " reason=" << ack.reason << "\n";

    if (mode == ClientMode::kDataBatch) {
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
      data_frame.header.message_id = next_message_id++;
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
      return 0;
    }

    if (mode == ClientMode::kCmd) {
      cstp::CommandRequestPayload command_request;
      command_request.command_id = 5001;
      command_request.target_device_id = hello.device_id;
      command_request.target_sensor_id = 65535;
      command_request.command_name = raw_command_name;
      command_request.timeout_ms = 2000;
      command_request.args_json = raw_args_json;

      cstp::Frame command_frame;
      command_frame.header.type = cstp::MessageType::kCmdReq;
      command_frame.header.flags = cstp::FrameFlags::kAckRequired;
      command_frame.header.message_id = next_message_id++;
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
      return 0;
    }

    if (mode == ClientMode::kPublish) {
      cstp::PublishPayload publish_request;
      publish_request.packet_id = 6001;
      publish_request.topic = publish_topic;
      publish_request.qos = publish_qos;
      publish_request.retain = publish_retain;
      publish_request.payload = std::vector<std::uint8_t>(publish_text.begin(), publish_text.end());

      cstp::Frame publish_frame;
      publish_frame.header.type = cstp::MessageType::kPublish;
      publish_frame.header.flags =
          publish_qos > 0 ? cstp::FrameFlags::kAckRequired : cstp::FrameFlags::kNone;
      publish_frame.header.message_id = next_message_id++;
      publish_frame.header.stream_id = cstp::StreamId::kData;
      publish_frame.header.timestamp_ms = cstp::UnixMillisNow();
      publish_frame.payload = cstp::EncodePublishPayload(publish_request);
      cstp::SendFrame(fd, publish_frame);

      std::cout << "PUBLISH sent topic=" << publish_request.topic
                << " qos=" << static_cast<int>(publish_request.qos)
                << " retain=" << (publish_request.retain ? 1 : 0)
                << " bytes=" << publish_request.payload.size() << "\n";

      if (publish_qos > 0) {
        const cstp::Frame puback_frame = cstp::ReceiveFrame(fd);
        if (puback_frame.header.type != cstp::MessageType::kPubAck) {
          throw cstp::ProtocolError("expected PUB_ACK after PUBLISH qos>0");
        }
        const cstp::PublishAckPayload puback = cstp::DecodePublishAckPayload(puback_frame.payload);
        std::cout << "PUB_ACK packet_id=" << puback.packet_id
                  << " status_code=" << puback.status_code
                  << " message=" << puback.message << "\n";
      }
      return 0;
    }

    if (mode == ClientMode::kSubscribe) {
      cstp::SubscribePayload sub_request;
      sub_request.packet_id = 7001;
      sub_request.topic_filter = subscribe_filter;
      sub_request.requested_qos = subscribe_qos;

      cstp::Frame sub_frame;
      sub_frame.header.type = cstp::MessageType::kSubscribe;
      sub_frame.header.flags = cstp::FrameFlags::kAckRequired;
      sub_frame.header.message_id = next_message_id++;
      sub_frame.header.stream_id = cstp::StreamId::kControl;
      sub_frame.header.timestamp_ms = cstp::UnixMillisNow();
      sub_frame.payload = cstp::EncodeSubscribePayload(sub_request);
      cstp::SendFrame(fd, sub_frame);

      const cstp::Frame suback_frame = cstp::ReceiveFrame(fd);
      if (suback_frame.header.type != cstp::MessageType::kSubAck) {
        throw cstp::ProtocolError("expected SUB_ACK after SUBSCRIBE");
      }
      const cstp::SubscribeAckPayload suback =
          cstp::DecodeSubscribeAckPayload(suback_frame.payload);
      std::cout << "SUB_ACK packet_id=" << suback.packet_id
                << " qos=" << static_cast<int>(suback.granted_qos)
                << " message=" << suback.message << "\n";
      std::cout << "listening for publishes on filter '" << subscribe_filter << "'\n";

      for (;;) {
        const cstp::Frame incoming = cstp::ReceiveFrame(fd);
        if (incoming.header.type == cstp::MessageType::kPublish) {
          const cstp::PublishPayload publish = cstp::DecodePublishPayload(incoming.payload);
          const std::string text(publish.payload.begin(), publish.payload.end());
          std::cout << "PUBLISH topic=" << publish.topic
                    << " qos=" << static_cast<int>(publish.qos)
                    << " retain=" << (publish.retain ? 1 : 0)
                    << " payload=\"" << text << "\"\n";

          if (publish.qos > 0) {
            cstp::PublishAckPayload puback;
            puback.packet_id = publish.packet_id;
            puback.status_code = 200;
            puback.message = "ok";

            cstp::Frame puback_frame;
            puback_frame.header.type = cstp::MessageType::kPubAck;
            puback_frame.header.flags = cstp::FrameFlags::kIsAck;
            puback_frame.header.message_id = next_message_id++;
            puback_frame.header.stream_id = cstp::StreamId::kControl;
            puback_frame.header.timestamp_ms = cstp::UnixMillisNow();
            puback_frame.payload = cstp::EncodePublishAckPayload(puback);
            cstp::SendFrame(fd, puback_frame);
          }
          continue;
        }

        if (incoming.header.type == cstp::MessageType::kError) {
          const cstp::ErrorPayload error = cstp::DecodeErrorPayload(incoming.payload);
          std::cout << "ERROR code=" << static_cast<std::uint16_t>(error.code)
                    << " message=" << error.message << "\n";
          continue;
        }

        std::cout << "ignoring unexpected frame type=" << cstp::ToString(incoming.header.type)
                  << "\n";
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "protocol error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
