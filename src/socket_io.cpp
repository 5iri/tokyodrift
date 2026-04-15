#include "cstp/socket_io.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>

#include <sys/socket.h>
#include <unistd.h>

namespace cstp {
namespace {

std::uint32_t ReadU32LE(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void ThrowSocketError(const std::string& operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

}  // namespace

void SendAll(int fd, std::span<const std::uint8_t> bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const ssize_t rc =
        send(fd, bytes.data() + static_cast<std::ptrdiff_t>(sent), bytes.size() - sent, 0);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowSocketError("send");
    }
    sent += static_cast<std::size_t>(rc);
  }
}

std::vector<std::uint8_t> ReceiveExact(int fd, std::size_t byte_count) {
  std::vector<std::uint8_t> out(byte_count);
  std::size_t received = 0;

  while (received < byte_count) {
    const ssize_t rc = recv(fd, out.data() + static_cast<std::ptrdiff_t>(received),
                            byte_count - received, 0);
    if (rc == 0) {
      throw ProtocolError("peer closed socket while receiving data");
    }
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowSocketError("recv");
    }
    received += static_cast<std::size_t>(rc);
  }

  return out;
}

void SendFrame(int fd, const Frame& frame, std::size_t max_frame_bytes) {
  const std::vector<std::uint8_t> wire = EncodeFrame(frame, max_frame_bytes);
  SendAll(fd, wire);
}

Frame ReceiveFrame(int fd, std::size_t max_frame_bytes) {
  if (max_frame_bytes < kMinFrameSize) {
    throw ProtocolError("max_frame_bytes is smaller than minimum frame size");
  }

  std::vector<std::uint8_t> wire = ReceiveExact(fd, kHeaderSize);

  const std::uint32_t payload_length = ReadU32LE(wire.data() + 19);
  const std::size_t total_length = kHeaderSize + static_cast<std::size_t>(payload_length) + kCrcSize;
  if (total_length > max_frame_bytes) {
    throw ProtocolError("incoming frame exceeds max_frame_bytes");
  }

  std::vector<std::uint8_t> tail =
      ReceiveExact(fd, static_cast<std::size_t>(payload_length) + kCrcSize);
  wire.insert(wire.end(), tail.begin(), tail.end());

  return DecodeFrame(wire, max_frame_bytes);
}

}  // namespace cstp
