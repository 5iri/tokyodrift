#include "cstp/protocol.hpp"

#include <array>
#include <chrono>
#include <limits>

namespace cstp {
namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw ProtocolError(message);
  }
}

void ValidateQos(std::uint8_t qos, const std::string& field_name) {
  Require(qos <= 2, field_name + " must be 0..2");
}

void AppendU8(std::vector<std::uint8_t>* out, std::uint8_t value) { out->push_back(value); }

void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void AppendU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void AppendU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 32) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 40) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 48) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 56) & 0xFF));
}

void AppendBytes(std::vector<std::uint8_t>* out, std::span<const std::uint8_t> bytes) {
  out->insert(out->end(), bytes.begin(), bytes.end());
}

void AppendString(std::vector<std::uint8_t>* out, const std::string& value) {
  Require(value.size() <= std::numeric_limits<std::uint16_t>::max(),
          "string too long for u16 length prefix");
  AppendU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void ValidateRawValue(ValueType type, std::span<const std::uint8_t> raw) {
  switch (type) {
    case ValueType::kInt64:
    case ValueType::kFloat64:
      Require(raw.size() == 8, "expected 8-byte sample value");
      break;
    case ValueType::kBool:
      Require(raw.size() == 1, "expected 1-byte bool sample value");
      break;
    case ValueType::kBytes:
    case ValueType::kString:
      Require(!raw.empty(), "byte/string sample value cannot be empty");
      Require(raw.size() <= std::numeric_limits<std::uint16_t>::max(),
              "byte/string sample value too long");
      break;
    default:
      throw ProtocolError("unsupported sample value type");
  }
}

class Cursor {
 public:
  explicit Cursor(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  std::size_t Remaining() const { return bytes_.size() - pos_; }

  std::uint8_t ReadU8() {
    Require(Remaining() >= 1, "unexpected end of payload while reading u8");
    return bytes_[pos_++];
  }

  std::uint16_t ReadU16() {
    Require(Remaining() >= 2, "unexpected end of payload while reading u16");
    const std::uint16_t value = static_cast<std::uint16_t>(bytes_[pos_]) |
                                (static_cast<std::uint16_t>(bytes_[pos_ + 1]) << 8);
    pos_ += 2;
    return value;
  }

  std::uint32_t ReadU32() {
    Require(Remaining() >= 4, "unexpected end of payload while reading u32");
    const std::uint32_t value = static_cast<std::uint32_t>(bytes_[pos_]) |
                                (static_cast<std::uint32_t>(bytes_[pos_ + 1]) << 8) |
                                (static_cast<std::uint32_t>(bytes_[pos_ + 2]) << 16) |
                                (static_cast<std::uint32_t>(bytes_[pos_ + 3]) << 24);
    pos_ += 4;
    return value;
  }

  std::uint64_t ReadU64() {
    Require(Remaining() >= 8, "unexpected end of payload while reading u64");
    const std::uint64_t value = static_cast<std::uint64_t>(bytes_[pos_]) |
                                (static_cast<std::uint64_t>(bytes_[pos_ + 1]) << 8) |
                                (static_cast<std::uint64_t>(bytes_[pos_ + 2]) << 16) |
                                (static_cast<std::uint64_t>(bytes_[pos_ + 3]) << 24) |
                                (static_cast<std::uint64_t>(bytes_[pos_ + 4]) << 32) |
                                (static_cast<std::uint64_t>(bytes_[pos_ + 5]) << 40) |
                                (static_cast<std::uint64_t>(bytes_[pos_ + 6]) << 48) |
                                (static_cast<std::uint64_t>(bytes_[pos_ + 7]) << 56);
    pos_ += 8;
    return value;
  }

  std::vector<std::uint8_t> ReadBytes(std::size_t count) {
    Require(Remaining() >= count, "unexpected end of payload while reading bytes");
    std::vector<std::uint8_t> out(bytes_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                  bytes_.begin() + static_cast<std::ptrdiff_t>(pos_ + count));
    pos_ += count;
    return out;
  }

  std::string ReadString() {
    const std::uint16_t size = ReadU16();
    Require(Remaining() >= size, "unexpected end of payload while reading string");
    const std::string out(bytes_.begin() + static_cast<std::ptrdiff_t>(pos_),
                          bytes_.begin() + static_cast<std::ptrdiff_t>(pos_ + size));
    pos_ += size;
    return out;
  }

  void RequireFinished(const std::string& context) const {
    Require(Remaining() == 0, context + " has trailing bytes");
  }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t pos_ = 0;
};

std::array<std::uint32_t, 256> MakeCrc32Table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    std::uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) {
      if ((value & 1u) != 0u) {
        value = (value >> 1u) ^ 0xEDB88320u;
      } else {
        value >>= 1u;
      }
    }
    table[i] = value;
  }
  return table;
}

const std::array<std::uint32_t, 256>& Crc32Table() {
  static const auto table = MakeCrc32Table();
  return table;
}

}  // namespace

FrameFlags operator|(FrameFlags left, FrameFlags right) {
  return static_cast<FrameFlags>(static_cast<std::uint8_t>(left) |
                                 static_cast<std::uint8_t>(right));
}

FrameFlags operator&(FrameFlags left, FrameFlags right) {
  return static_cast<FrameFlags>(static_cast<std::uint8_t>(left) &
                                 static_cast<std::uint8_t>(right));
}

bool HasFlag(FrameFlags value, FrameFlags flag) {
  return (value & flag) == flag;
}

std::string ToString(MessageType type) {
  switch (type) {
    case MessageType::kHello:
      return "HELLO";
    case MessageType::kHelloAck:
      return "HELLO_ACK";
    case MessageType::kDataBatch:
      return "DATA_BATCH";
    case MessageType::kDataAck:
      return "DATA_ACK";
    case MessageType::kCmdReq:
      return "CMD_REQ";
    case MessageType::kCmdResp:
      return "CMD_RESP";
    case MessageType::kPublish:
      return "PUBLISH";
    case MessageType::kPubAck:
      return "PUB_ACK";
    case MessageType::kSubscribe:
      return "SUBSCRIBE";
    case MessageType::kSubAck:
      return "SUB_ACK";
    case MessageType::kUnsubscribe:
      return "UNSUBSCRIBE";
    case MessageType::kUnsubAck:
      return "UNSUB_ACK";
    case MessageType::kHeartbeat:
      return "HEARTBEAT";
    case MessageType::kError:
      return "ERROR";
    default:
      return "UNKNOWN";
  }
}

bool IsSupportedValueType(ValueType type) {
  switch (type) {
    case ValueType::kInt64:
    case ValueType::kFloat64:
    case ValueType::kBool:
    case ValueType::kBytes:
    case ValueType::kString:
      return true;
    default:
      return false;
  }
}

void ValidateLimits(const Limits& limits) {
  Require(limits.max_sensors_per_batch > 0, "max_sensors_per_batch must be > 0");
  Require(limits.max_samples_per_sensor > 0, "max_samples_per_sensor must be > 0");
  Require(limits.send_interval_ms > 0, "send_interval_ms must be > 0");
  Require(limits.max_frame_bytes >= kMinFrameSize,
          "max_frame_bytes must be >= minimum frame size");
  Require(limits.ack_timeout_ms > 0, "ack_timeout_ms must be > 0");
  Require(limits.max_retries >= 0, "max_retries must be >= 0");
}

void ValidateFrameHeader(const FrameHeader& header, std::size_t max_frame_bytes) {
  Require(header.magic == kMagic, "invalid magic");
  Require(header.version == kVersionV0, "unsupported protocol version");
  Require(max_frame_bytes >= kMinFrameSize, "max_frame_bytes too small");
  const std::size_t total_size = kMinFrameSize + static_cast<std::size_t>(header.payload_length);
  Require(total_size <= max_frame_bytes, "frame size exceeds configured max_frame_bytes");
}

void ValidateDataBatchPayload(const DataBatchPayload& payload, const Limits& limits) {
  ValidateLimits(limits);
  Require(!payload.device_id.empty(), "DATA_BATCH requires a device_id");
  Require(!payload.sensors.empty(), "DATA_BATCH requires at least one sensor series");
  Require(static_cast<int>(payload.sensors.size()) <= limits.max_sensors_per_batch,
          "DATA_BATCH sensor_count exceeds max_sensors_per_batch");

  for (std::size_t i = 0; i < payload.sensors.size(); ++i) {
    const auto& sensor = payload.sensors[i];
    Require(!sensor.metric_name.empty(), "sensor metric_name is required");
    Require(IsSupportedValueType(sensor.value_type), "unsupported sensor value_type");
    Require(!sensor.samples.empty(), "sensor must have at least one sample");
    Require(static_cast<int>(sensor.samples.size()) <= limits.max_samples_per_sensor,
            "sensor sample_count exceeds max_samples_per_sensor");

    for (const auto& sample : sensor.samples) {
      ValidateRawValue(sensor.value_type, sample.value);
    }
  }
}

std::size_t MaxPayloadBytes(std::size_t max_frame_bytes) {
  Require(max_frame_bytes >= kMinFrameSize, "max_frame_bytes smaller than minimum frame size");
  return max_frame_bytes - kMinFrameSize;
}

std::uint64_t UnixMillisNow() {
  const auto now = std::chrono::system_clock::now();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

std::uint32_t ComputeCrc32(std::span<const std::uint8_t> bytes) {
  std::uint32_t crc = 0xFFFFFFFFu;
  const auto& table = Crc32Table();
  for (const std::uint8_t byte : bytes) {
    const std::uint8_t lookup = static_cast<std::uint8_t>((crc ^ byte) & 0xFFu);
    crc = (crc >> 8u) ^ table[lookup];
  }
  return crc ^ 0xFFFFFFFFu;
}

std::vector<std::uint8_t> EncodeFrame(const Frame& frame, std::size_t max_frame_bytes) {
  FrameHeader header = frame.header;
  header.payload_length = static_cast<std::uint32_t>(frame.payload.size());
  ValidateFrameHeader(header, max_frame_bytes);

  std::vector<std::uint8_t> wire;
  wire.reserve(kHeaderSize + frame.payload.size() + kCrcSize);

  AppendU16(&wire, header.magic);
  AppendU8(&wire, header.version);
  AppendU8(&wire, static_cast<std::uint8_t>(header.type));
  AppendU8(&wire, static_cast<std::uint8_t>(header.flags));
  AppendU32(&wire, header.message_id);
  AppendU16(&wire, static_cast<std::uint16_t>(header.stream_id));
  AppendU64(&wire, header.timestamp_ms);
  AppendU32(&wire, header.payload_length);
  AppendBytes(&wire, frame.payload);

  const std::uint32_t crc = ComputeCrc32(wire);
  AppendU32(&wire, crc);

  Require(wire.size() <= max_frame_bytes, "encoded frame exceeds max_frame_bytes");
  return wire;
}

Frame DecodeFrame(std::span<const std::uint8_t> wire, std::size_t max_frame_bytes) {
  Require(wire.size() >= kMinFrameSize, "frame too short");

  Cursor cur(wire);
  Frame out;
  out.header.magic = cur.ReadU16();
  out.header.version = cur.ReadU8();
  out.header.type = static_cast<MessageType>(cur.ReadU8());
  out.header.flags = static_cast<FrameFlags>(cur.ReadU8());
  out.header.message_id = cur.ReadU32();
  out.header.stream_id = static_cast<StreamId>(cur.ReadU16());
  out.header.timestamp_ms = cur.ReadU64();
  out.header.payload_length = cur.ReadU32();

  const std::size_t expected_size = kHeaderSize + static_cast<std::size_t>(out.header.payload_length) +
                                    kCrcSize;
  Require(expected_size == wire.size(), "frame length does not match payload_length");
  ValidateFrameHeader(out.header, max_frame_bytes);

  out.payload = cur.ReadBytes(out.header.payload_length);
  const std::uint32_t received_crc = cur.ReadU32();
  cur.RequireFinished("frame");

  const std::span<const std::uint8_t> crc_input(wire.data(), wire.size() - kCrcSize);
  const std::uint32_t expected_crc = ComputeCrc32(crc_input);
  Require(received_crc == expected_crc, "frame CRC mismatch");

  return out;
}

std::vector<std::uint8_t> EncodeHelloPayload(const HelloPayload& payload) {
  std::vector<std::uint8_t> bytes;
  AppendString(&bytes, payload.device_id);
  AppendString(&bytes, payload.auth_token);
  AppendU16(&bytes, payload.requested_keepalive_sec);
  AppendU32(&bytes, payload.client_max_frame_bytes);
  AppendString(&bytes, payload.client_version_name);
  return bytes;
}

HelloPayload DecodeHelloPayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  HelloPayload out;
  out.device_id = cur.ReadString();
  out.auth_token = cur.ReadString();
  out.requested_keepalive_sec = cur.ReadU16();
  out.client_max_frame_bytes = cur.ReadU32();
  out.client_version_name = cur.ReadString();
  cur.RequireFinished("HELLO payload");
  return out;
}

std::vector<std::uint8_t> EncodeHelloAckPayload(const HelloAckPayload& payload) {
  std::vector<std::uint8_t> bytes;
  AppendU8(&bytes, payload.accepted ? 1 : 0);
  AppendU32(&bytes, payload.session_id);
  AppendU64(&bytes, payload.server_time_ms);
  AppendU16(&bytes, payload.accepted_keepalive_sec);
  AppendU32(&bytes, payload.accepted_max_frame_bytes);
  AppendString(&bytes, payload.reason);
  return bytes;
}

HelloAckPayload DecodeHelloAckPayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  HelloAckPayload out;
  out.accepted = cur.ReadU8() == 1;
  out.session_id = cur.ReadU32();
  out.server_time_ms = cur.ReadU64();
  out.accepted_keepalive_sec = cur.ReadU16();
  out.accepted_max_frame_bytes = cur.ReadU32();
  out.reason = cur.ReadString();
  cur.RequireFinished("HELLO_ACK payload");
  return out;
}

std::vector<std::uint8_t> EncodeDataBatchPayload(const DataBatchPayload& payload,
                                                 const Limits& limits) {
  ValidateDataBatchPayload(payload, limits);

  std::vector<std::uint8_t> bytes;
  AppendU32(&bytes, payload.batch_id);
  AppendString(&bytes, payload.device_id);
  AppendU64(&bytes, payload.batch_time_ms);
  AppendU16(&bytes, static_cast<std::uint16_t>(payload.sensors.size()));

  for (const auto& sensor : payload.sensors) {
    AppendU16(&bytes, sensor.sensor_id);
    AppendString(&bytes, sensor.metric_name);
    AppendU8(&bytes, static_cast<std::uint8_t>(sensor.value_type));
    AppendU16(&bytes, static_cast<std::uint16_t>(sensor.samples.size()));

    for (const auto& sample : sensor.samples) {
      AppendU32(&bytes, sample.delta_ms);
      if (sensor.value_type == ValueType::kBytes || sensor.value_type == ValueType::kString) {
        AppendU16(&bytes, static_cast<std::uint16_t>(sample.value.size()));
      }
      AppendBytes(&bytes, sample.value);
    }
  }

  return bytes;
}

DataBatchPayload DecodeDataBatchPayload(std::span<const std::uint8_t> bytes, const Limits& limits) {
  ValidateLimits(limits);

  Cursor cur(bytes);
  DataBatchPayload out;
  out.batch_id = cur.ReadU32();
  out.device_id = cur.ReadString();
  out.batch_time_ms = cur.ReadU64();

  const std::uint16_t sensor_count = cur.ReadU16();
  Require(sensor_count > 0, "DATA_BATCH sensor_count must be > 0");
  Require(sensor_count <= limits.max_sensors_per_batch,
          "DATA_BATCH sensor_count exceeds max_sensors_per_batch");
  out.sensors.reserve(sensor_count);

  for (std::uint16_t i = 0; i < sensor_count; ++i) {
    SensorSeries sensor;
    sensor.sensor_id = cur.ReadU16();
    sensor.metric_name = cur.ReadString();
    sensor.value_type = static_cast<ValueType>(cur.ReadU8());
    Require(IsSupportedValueType(sensor.value_type), "DATA_BATCH has unsupported value_type");

    const std::uint16_t sample_count = cur.ReadU16();
    Require(sample_count > 0, "sample_count must be > 0");
    Require(sample_count <= limits.max_samples_per_sensor,
            "sample_count exceeds max_samples_per_sensor");
    sensor.samples.reserve(sample_count);

    for (std::uint16_t j = 0; j < sample_count; ++j) {
      Sample sample;
      sample.delta_ms = cur.ReadU32();
      switch (sensor.value_type) {
        case ValueType::kInt64:
        case ValueType::kFloat64:
          sample.value = cur.ReadBytes(8);
          break;
        case ValueType::kBool:
          sample.value = cur.ReadBytes(1);
          break;
        case ValueType::kBytes:
        case ValueType::kString: {
          const std::uint16_t value_size = cur.ReadU16();
          sample.value = cur.ReadBytes(value_size);
          break;
        }
        default:
          throw ProtocolError("unsupported value_type");
      }
      ValidateRawValue(sensor.value_type, sample.value);
      sensor.samples.push_back(std::move(sample));
    }

    out.sensors.push_back(std::move(sensor));
  }

  cur.RequireFinished("DATA_BATCH payload");
  ValidateDataBatchPayload(out, limits);
  return out;
}

std::vector<std::uint8_t> EncodeDataAckPayload(const DataAckPayload& payload) {
  std::vector<std::uint8_t> bytes;
  AppendU32(&bytes, payload.acked_message_id);
  AppendU32(&bytes, payload.batch_id);
  AppendU8(&bytes, static_cast<std::uint8_t>(payload.status));
  AppendU64(&bytes, payload.received_at_ms);
  AppendString(&bytes, payload.note);
  return bytes;
}

DataAckPayload DecodeDataAckPayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  DataAckPayload out;
  out.acked_message_id = cur.ReadU32();
  out.batch_id = cur.ReadU32();
  out.status = static_cast<DataAckStatus>(cur.ReadU8());
  out.received_at_ms = cur.ReadU64();
  out.note = cur.ReadString();
  cur.RequireFinished("DATA_ACK payload");
  return out;
}

std::vector<std::uint8_t> EncodeCommandRequestPayload(const CommandRequestPayload& payload) {
  std::vector<std::uint8_t> bytes;
  AppendU32(&bytes, payload.command_id);
  AppendString(&bytes, payload.target_device_id);
  AppendU16(&bytes, payload.target_sensor_id);
  AppendString(&bytes, payload.command_name);
  AppendString(&bytes, payload.args_json);
  AppendU32(&bytes, payload.timeout_ms);
  return bytes;
}

CommandRequestPayload DecodeCommandRequestPayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  CommandRequestPayload out;
  out.command_id = cur.ReadU32();
  out.target_device_id = cur.ReadString();
  out.target_sensor_id = cur.ReadU16();
  out.command_name = cur.ReadString();
  out.args_json = cur.ReadString();
  out.timeout_ms = cur.ReadU32();
  cur.RequireFinished("CMD_REQ payload");
  return out;
}

std::vector<std::uint8_t> EncodeCommandResponsePayload(const CommandResponsePayload& payload) {
  std::vector<std::uint8_t> bytes;
  AppendU32(&bytes, payload.command_id);
  AppendU16(&bytes, payload.status_code);
  AppendString(&bytes, payload.message);
  AppendString(&bytes, payload.result_json);
  return bytes;
}

CommandResponsePayload DecodeCommandResponsePayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  CommandResponsePayload out;
  out.command_id = cur.ReadU32();
  out.status_code = cur.ReadU16();
  out.message = cur.ReadString();
  out.result_json = cur.ReadString();
  cur.RequireFinished("CMD_RESP payload");
  return out;
}

std::vector<std::uint8_t> EncodePublishPayload(const PublishPayload& payload) {
  ValidateQos(payload.qos, "PUBLISH qos");
  Require(!payload.topic.empty(), "PUBLISH requires topic");
  Require(payload.payload.size() <= std::numeric_limits<std::uint32_t>::max(),
          "PUBLISH payload too large");

  std::vector<std::uint8_t> bytes;
  AppendU32(&bytes, payload.packet_id);
  AppendString(&bytes, payload.topic);
  AppendU8(&bytes, payload.qos);
  AppendU8(&bytes, payload.retain ? 1 : 0);
  AppendU32(&bytes, static_cast<std::uint32_t>(payload.payload.size()));
  AppendBytes(&bytes, payload.payload);
  return bytes;
}

PublishPayload DecodePublishPayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  PublishPayload out;
  out.packet_id = cur.ReadU32();
  out.topic = cur.ReadString();
  out.qos = cur.ReadU8();
  ValidateQos(out.qos, "PUBLISH qos");
  out.retain = cur.ReadU8() == 1;
  const std::uint32_t payload_size = cur.ReadU32();
  out.payload = cur.ReadBytes(payload_size);
  Require(!out.topic.empty(), "PUBLISH requires topic");
  cur.RequireFinished("PUBLISH payload");
  return out;
}

std::vector<std::uint8_t> EncodePublishAckPayload(const PublishAckPayload& payload) {
  std::vector<std::uint8_t> bytes;
  AppendU32(&bytes, payload.packet_id);
  AppendU16(&bytes, payload.status_code);
  AppendString(&bytes, payload.message);
  return bytes;
}

PublishAckPayload DecodePublishAckPayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  PublishAckPayload out;
  out.packet_id = cur.ReadU32();
  out.status_code = cur.ReadU16();
  out.message = cur.ReadString();
  cur.RequireFinished("PUB_ACK payload");
  return out;
}

std::vector<std::uint8_t> EncodeSubscribePayload(const SubscribePayload& payload) {
  ValidateQos(payload.requested_qos, "SUBSCRIBE requested_qos");
  Require(!payload.topic_filter.empty(), "SUBSCRIBE requires topic_filter");

  std::vector<std::uint8_t> bytes;
  AppendU32(&bytes, payload.packet_id);
  AppendString(&bytes, payload.topic_filter);
  AppendU8(&bytes, payload.requested_qos);
  return bytes;
}

SubscribePayload DecodeSubscribePayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  SubscribePayload out;
  out.packet_id = cur.ReadU32();
  out.topic_filter = cur.ReadString();
  out.requested_qos = cur.ReadU8();
  ValidateQos(out.requested_qos, "SUBSCRIBE requested_qos");
  Require(!out.topic_filter.empty(), "SUBSCRIBE requires topic_filter");
  cur.RequireFinished("SUBSCRIBE payload");
  return out;
}

std::vector<std::uint8_t> EncodeSubscribeAckPayload(const SubscribeAckPayload& payload) {
  ValidateQos(payload.granted_qos, "SUB_ACK granted_qos");

  std::vector<std::uint8_t> bytes;
  AppendU32(&bytes, payload.packet_id);
  AppendU8(&bytes, payload.granted_qos);
  AppendString(&bytes, payload.message);
  return bytes;
}

SubscribeAckPayload DecodeSubscribeAckPayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  SubscribeAckPayload out;
  out.packet_id = cur.ReadU32();
  out.granted_qos = cur.ReadU8();
  ValidateQos(out.granted_qos, "SUB_ACK granted_qos");
  out.message = cur.ReadString();
  cur.RequireFinished("SUB_ACK payload");
  return out;
}

std::vector<std::uint8_t> EncodeUnsubscribePayload(const UnsubscribePayload& payload) {
  Require(!payload.topic_filter.empty(), "UNSUBSCRIBE requires topic_filter");

  std::vector<std::uint8_t> bytes;
  AppendU32(&bytes, payload.packet_id);
  AppendString(&bytes, payload.topic_filter);
  return bytes;
}

UnsubscribePayload DecodeUnsubscribePayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  UnsubscribePayload out;
  out.packet_id = cur.ReadU32();
  out.topic_filter = cur.ReadString();
  Require(!out.topic_filter.empty(), "UNSUBSCRIBE requires topic_filter");
  cur.RequireFinished("UNSUBSCRIBE payload");
  return out;
}

std::vector<std::uint8_t> EncodeUnsubscribeAckPayload(const UnsubscribeAckPayload& payload) {
  std::vector<std::uint8_t> bytes;
  AppendU32(&bytes, payload.packet_id);
  AppendString(&bytes, payload.message);
  return bytes;
}

UnsubscribeAckPayload DecodeUnsubscribeAckPayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  UnsubscribeAckPayload out;
  out.packet_id = cur.ReadU32();
  out.message = cur.ReadString();
  cur.RequireFinished("UNSUB_ACK payload");
  return out;
}

std::vector<std::uint8_t> EncodeHeartbeatPayload(const HeartbeatPayload& payload) {
  std::vector<std::uint8_t> bytes;
  AppendU64(&bytes, payload.uptime_ms);
  AppendU16(&bytes, payload.pending_batches);
  AppendU32(&bytes, payload.last_batch_id);
  return bytes;
}

HeartbeatPayload DecodeHeartbeatPayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  HeartbeatPayload out;
  out.uptime_ms = cur.ReadU64();
  out.pending_batches = cur.ReadU16();
  out.last_batch_id = cur.ReadU32();
  cur.RequireFinished("HEARTBEAT payload");
  return out;
}

std::vector<std::uint8_t> EncodeErrorPayload(const ErrorPayload& payload) {
  std::vector<std::uint8_t> bytes;
  AppendU16(&bytes, static_cast<std::uint16_t>(payload.code));
  AppendU32(&bytes, payload.related_message_id);
  AppendString(&bytes, payload.message);
  return bytes;
}

ErrorPayload DecodeErrorPayload(std::span<const std::uint8_t> bytes) {
  Cursor cur(bytes);
  ErrorPayload out;
  out.code = static_cast<ErrorCode>(cur.ReadU16());
  out.related_message_id = cur.ReadU32();
  out.message = cur.ReadString();
  cur.RequireFinished("ERROR payload");
  return out;
}

}  // namespace cstp
