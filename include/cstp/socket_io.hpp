#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cstp/protocol.hpp"

namespace cstp {

void SendAll(int fd, std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> ReceiveExact(int fd, std::size_t byte_count);

void SendFrame(int fd, const Frame& frame,
               std::size_t max_frame_bytes = kDefaultMaxFrameBytes);
Frame ReceiveFrame(int fd, std::size_t max_frame_bytes = kDefaultMaxFrameBytes);

}  // namespace cstp
