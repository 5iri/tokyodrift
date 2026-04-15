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

  sockaddr_in client_addr{};
  socklen_t client_len = sizeof(client_addr);
  const int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
  if (client_fd < 0) {
    std::cerr << "accept() failed\n";
    return 1;
  }
  FdGuard client_guard(client_fd);

  try {
    const cstp::Frame request = cstp::ReceiveFrame(client_fd);
    if (request.header.type != cstp::MessageType::kHello) {
      throw cstp::ProtocolError("expected HELLO as first message");
    }

    const cstp::HelloPayload hello = cstp::DecodeHelloPayload(request.payload);
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

    cstp::Frame response;
    response.header.type = cstp::MessageType::kHelloAck;
    response.header.flags = cstp::FrameFlags::kIsAck;
    response.header.message_id = request.header.message_id;
    response.header.stream_id = cstp::StreamId::kControl;
    response.header.timestamp_ms = cstp::UnixMillisNow();
    response.payload = cstp::EncodeHelloAckPayload(ack);

    cstp::SendFrame(client_fd, response);
    std::cout << "HELLO_ACK sent\n";
  } catch (const std::exception& e) {
    std::cerr << "protocol error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
