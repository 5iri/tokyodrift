#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace cstp {

constexpr std::uint16_t kMagic = 0x4350;
constexpr std::uint8_t kVersionV0 = 1;
constexpr std::size_t kHeaderSize = 23;
constexpr std::size_t kCrcSize = 4;
constexpr std::size_t kMinFrameSize = kHeaderSize + kCrcSize;

constexpr int kDefaultMaxSensorsPerBatch = 64;
constexpr int kDefaultMaxSamplesPerSensor = 200;
constexpr int kDefaultSendIntervalMs = 5000;
constexpr std::size_t kDefaultMaxFrameBytes = 1'048'576;
constexpr int kDefaultAckTimeoutMs = 2000;
constexpr int kDefaultMaxRetries = 3;

struct Limits {
  int max_sensors_per_batch = kDefaultMaxSensorsPerBatch;
  int max_samples_per_sensor = kDefaultMaxSamplesPerSensor;
  int send_interval_ms = kDefaultSendIntervalMs;
  std::size_t max_frame_bytes = kDefaultMaxFrameBytes;
  int ack_timeout_ms = kDefaultAckTimeoutMs;
  int max_retries = kDefaultMaxRetries;
};

enum class StreamId : std::uint16_t {
  kControl = 0,
  kData = 1,
};

enum class MessageType : std::uint8_t {
  kHello = 0x01,
  kHelloAck = 0x02,
  kDataBatch = 0x10,
  kDataAck = 0x11,
  kCmdReq = 0x20,
  kCmdResp = 0x21,
  kPublish = 0x40,
  kPubAck = 0x41,
  kSubscribe = 0x42,
  kSubAck = 0x43,
  kUnsubscribe = 0x44,
  kUnsubAck = 0x45,
  kHeartbeat = 0x30,
  kError = 0x7F,
};

enum class FrameFlags : std::uint8_t {
  kNone = 0x00,
  kAckRequired = 0x01,
  kIsAck = 0x02,
  kCompressed = 0x04,
};

enum class ValueType : std::uint8_t {
  kInt64 = 1,
  kFloat64 = 2,
  kBool = 3,
  kBytes = 4,
  kString = 5,
};

enum class DataAckStatus : std::uint8_t {
  kAccepted = 0,
  kDuplicateAccepted = 1,
  kRejected = 2,
};

enum class ErrorCode : std::uint16_t {
  kMalformedFrame = 1001,
  kUnsupportedVersion = 1002,
  kAuthFailed = 1003,
  kPayloadTooLarge = 1004,
  kDecodeFailed = 1005,
  kUnknownMessageType = 1006,
  kRateLimited = 1007,
  kInternalError = 1008,
};

class ProtocolError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct FrameHeader {
  std::uint16_t magic = kMagic;
  std::uint8_t version = kVersionV0;
  MessageType type = MessageType::kHello;
  FrameFlags flags = FrameFlags::kNone;
  std::uint32_t message_id = 0;
  StreamId stream_id = StreamId::kControl;
  std::uint64_t timestamp_ms = 0;
  std::uint32_t payload_length = 0;
};

struct Frame {
  FrameHeader header;
  std::vector<std::uint8_t> payload;
};

struct HelloPayload {
  std::string device_id;
  std::string auth_token;
  std::uint16_t requested_keepalive_sec = 0;
  std::uint32_t client_max_frame_bytes = 0;
  std::string client_version_name;
};

struct HelloAckPayload {
  bool accepted = false;
  std::uint32_t session_id = 0;
  std::uint64_t server_time_ms = 0;
  std::uint16_t accepted_keepalive_sec = 0;
  std::uint32_t accepted_max_frame_bytes = 0;
  std::string reason;
};

struct Sample {
  std::uint32_t delta_ms = 0;
  std::vector<std::uint8_t> value;
};

struct SensorSeries {
  std::uint16_t sensor_id = 0;
  std::string metric_name;
  ValueType value_type = ValueType::kFloat64;
  std::vector<Sample> samples;
};

struct DataBatchPayload {
  std::uint32_t batch_id = 0;
  std::string device_id;
  std::uint64_t batch_time_ms = 0;
  std::vector<SensorSeries> sensors;
};

struct DataAckPayload {
  std::uint32_t acked_message_id = 0;
  std::uint32_t batch_id = 0;
  DataAckStatus status = DataAckStatus::kAccepted;
  std::uint64_t received_at_ms = 0;
  std::string note;
};

struct CommandRequestPayload {
  std::uint32_t command_id = 0;
  std::string target_device_id;
  std::uint16_t target_sensor_id = 0;
  std::string command_name;
  std::string args_json;
  std::uint32_t timeout_ms = 0;
};

struct CommandResponsePayload {
  std::uint32_t command_id = 0;
  std::uint16_t status_code = 0;
  std::string message;
  std::string result_json;
};

struct PublishPayload {
  std::uint32_t packet_id = 0;
  std::string topic;
  std::uint8_t qos = 0;
  bool retain = false;
  std::vector<std::uint8_t> payload;
};

struct PublishAckPayload {
  std::uint32_t packet_id = 0;
  std::uint16_t status_code = 0;
  std::string message;
};

struct SubscribePayload {
  std::uint32_t packet_id = 0;
  std::string topic_filter;
  std::uint8_t requested_qos = 0;
};

struct SubscribeAckPayload {
  std::uint32_t packet_id = 0;
  std::uint8_t granted_qos = 0;
  std::string message;
};

struct UnsubscribePayload {
  std::uint32_t packet_id = 0;
  std::string topic_filter;
};

struct UnsubscribeAckPayload {
  std::uint32_t packet_id = 0;
  std::string message;
};

struct HeartbeatPayload {
  std::uint64_t uptime_ms = 0;
  std::uint16_t pending_batches = 0;
  std::uint32_t last_batch_id = 0;
};

struct ErrorPayload {
  ErrorCode code = ErrorCode::kInternalError;
  std::uint32_t related_message_id = 0;
  std::string message;
};

FrameFlags operator|(FrameFlags left, FrameFlags right);
FrameFlags operator&(FrameFlags left, FrameFlags right);
bool HasFlag(FrameFlags value, FrameFlags flag);

std::string ToString(MessageType type);
bool IsSupportedValueType(ValueType type);

void ValidateLimits(const Limits& limits);
void ValidateFrameHeader(const FrameHeader& header, std::size_t max_frame_bytes);
void ValidateDataBatchPayload(const DataBatchPayload& payload, const Limits& limits);

std::size_t MaxPayloadBytes(std::size_t max_frame_bytes);
std::uint64_t UnixMillisNow();

std::uint32_t ComputeCrc32(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodeFrame(const Frame& frame,
                                      std::size_t max_frame_bytes = kDefaultMaxFrameBytes);
Frame DecodeFrame(std::span<const std::uint8_t> wire,
                  std::size_t max_frame_bytes = kDefaultMaxFrameBytes);

std::vector<std::uint8_t> EncodeHelloPayload(const HelloPayload& payload);
HelloPayload DecodeHelloPayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodeHelloAckPayload(const HelloAckPayload& payload);
HelloAckPayload DecodeHelloAckPayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodeDataBatchPayload(const DataBatchPayload& payload,
                                                 const Limits& limits = Limits{});
DataBatchPayload DecodeDataBatchPayload(std::span<const std::uint8_t> bytes,
                                        const Limits& limits = Limits{});

std::vector<std::uint8_t> EncodeDataAckPayload(const DataAckPayload& payload);
DataAckPayload DecodeDataAckPayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodeCommandRequestPayload(const CommandRequestPayload& payload);
CommandRequestPayload DecodeCommandRequestPayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodeCommandResponsePayload(const CommandResponsePayload& payload);
CommandResponsePayload DecodeCommandResponsePayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodePublishPayload(const PublishPayload& payload);
PublishPayload DecodePublishPayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodePublishAckPayload(const PublishAckPayload& payload);
PublishAckPayload DecodePublishAckPayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodeSubscribePayload(const SubscribePayload& payload);
SubscribePayload DecodeSubscribePayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodeSubscribeAckPayload(const SubscribeAckPayload& payload);
SubscribeAckPayload DecodeSubscribeAckPayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodeUnsubscribePayload(const UnsubscribePayload& payload);
UnsubscribePayload DecodeUnsubscribePayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodeUnsubscribeAckPayload(const UnsubscribeAckPayload& payload);
UnsubscribeAckPayload DecodeUnsubscribeAckPayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodeHeartbeatPayload(const HeartbeatPayload& payload);
HeartbeatPayload DecodeHeartbeatPayload(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> EncodeErrorPayload(const ErrorPayload& payload);
ErrorPayload DecodeErrorPayload(std::span<const std::uint8_t> bytes);

}  // namespace cstp
