#include <cstdlib>
#include <iostream>
#include <string>

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

int main(int argc, char** argv) {
  const std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
  const std::string port = (argc > 2) ? argv[2] : "18830";

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
  } catch (const std::exception& e) {
    std::cerr << "protocol error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
