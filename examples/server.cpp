#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <arpa/inet.h>
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

bool IsClientDisconnect(const std::exception& e) {
  return std::string(e.what()) == "peer closed socket while receiving data";
}

void LogFrameSummary(const cstp::Frame& frame) {
  std::cout << "received " << cstp::ToString(frame.header.type) << " msg_id="
            << frame.header.message_id << " payload_bytes=" << frame.payload.size() << "\n";
}

void HandleClient(int client_fd) {
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
        break;
      }
      case cstp::MessageType::kHeartbeat: {
        const cstp::HeartbeatPayload heartbeat = cstp::DecodeHeartbeatPayload(frame.payload);
        std::cout << "HEARTBEAT uptime_ms=" << heartbeat.uptime_ms
                  << " pending_batches=" << heartbeat.pending_batches
                  << " last_batch_id=" << heartbeat.last_batch_id << "\n";
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
      HandleClient(client_fd);
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
