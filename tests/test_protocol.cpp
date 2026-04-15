#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "cstp/protocol.hpp"

namespace {

bool ExpectProtocolError(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const cstp::ProtocolError&) {
    return true;
  }
  return false;
}

void TestFrameRoundTrip() {
  cstp::HelloPayload hello;
  hello.device_id = "device-01";
  hello.auth_token = "secret-token";
  hello.requested_keepalive_sec = 30;
  hello.client_max_frame_bytes = static_cast<std::uint32_t>(cstp::kDefaultMaxFrameBytes);
  hello.client_version_name = "cstp-cpp/0.1";

  cstp::Frame frame;
  frame.header.type = cstp::MessageType::kHello;
  frame.header.flags = cstp::FrameFlags::kAckRequired;
  frame.header.message_id = 42;
  frame.header.stream_id = cstp::StreamId::kControl;
  frame.header.timestamp_ms = cstp::UnixMillisNow();
  frame.payload = cstp::EncodeHelloPayload(hello);

  const std::vector<std::uint8_t> wire = cstp::EncodeFrame(frame);
  const cstp::Frame decoded = cstp::DecodeFrame(wire);
  const cstp::HelloPayload decoded_hello = cstp::DecodeHelloPayload(decoded.payload);

  assert(decoded.header.message_id == frame.header.message_id);
  assert(decoded.header.type == cstp::MessageType::kHello);
  assert(decoded_hello.device_id == hello.device_id);
  assert(decoded_hello.auth_token == hello.auth_token);
  assert(decoded_hello.requested_keepalive_sec == hello.requested_keepalive_sec);
  assert(decoded_hello.client_version_name == hello.client_version_name);
}

void TestCrcMismatchRejected() {
  cstp::Frame frame;
  frame.header.type = cstp::MessageType::kHeartbeat;
  frame.header.message_id = 7;
  frame.header.stream_id = cstp::StreamId::kControl;
  frame.header.timestamp_ms = cstp::UnixMillisNow();
  cstp::HeartbeatPayload heartbeat;
  heartbeat.uptime_ms = 10'000;
  heartbeat.pending_batches = 0;
  heartbeat.last_batch_id = 0;
  frame.payload = cstp::EncodeHeartbeatPayload(heartbeat);

  std::vector<std::uint8_t> wire = cstp::EncodeFrame(frame);
  wire[10] ^= 0x01;

  const bool thrown = ExpectProtocolError([&wire]() { (void)cstp::DecodeFrame(wire); });
  assert(thrown);
}

void TestDataBatchRoundTrip() {
  cstp::DataBatchPayload payload;
  payload.batch_id = 99;
  payload.device_id = "gateway-a";
  payload.batch_time_ms = cstp::UnixMillisNow();

  cstp::SensorSeries temperature;
  temperature.sensor_id = 1;
  temperature.metric_name = "temperature";
  temperature.value_type = cstp::ValueType::kFloat64;
  temperature.samples.push_back(cstp::Sample{.delta_ms = 0,
                                             .value = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 240, 63}});
  temperature.samples.push_back(cstp::Sample{.delta_ms = 1000,
                                             .value = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 4, 64}});

  cstp::SensorSeries online;
  online.sensor_id = 2;
  online.metric_name = "online";
  online.value_type = cstp::ValueType::kBool;
  online.samples.push_back(cstp::Sample{.delta_ms = 0, .value = std::vector<std::uint8_t>{1}});

  payload.sensors = {temperature, online};

  const std::vector<std::uint8_t> encoded = cstp::EncodeDataBatchPayload(payload);
  const cstp::DataBatchPayload decoded = cstp::DecodeDataBatchPayload(encoded);

  assert(decoded.batch_id == payload.batch_id);
  assert(decoded.device_id == payload.device_id);
  assert(decoded.sensors.size() == payload.sensors.size());
  assert(decoded.sensors[0].samples.size() == payload.sensors[0].samples.size());
  assert(decoded.sensors[1].samples[0].value[0] == 1);
}

void TestBrokerPayloadRoundTrip() {
  cstp::PublishPayload publish;
  publish.packet_id = 9001;
  publish.topic = "devices/lab-1/status";
  publish.qos = 1;
  publish.retain = true;
  publish.payload = std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o'};

  const std::vector<std::uint8_t> publish_encoded = cstp::EncodePublishPayload(publish);
  const cstp::PublishPayload publish_decoded = cstp::DecodePublishPayload(publish_encoded);
  assert(publish_decoded.packet_id == publish.packet_id);
  assert(publish_decoded.topic == publish.topic);
  assert(publish_decoded.qos == publish.qos);
  assert(publish_decoded.retain == publish.retain);
  assert(publish_decoded.payload == publish.payload);

  cstp::SubscribePayload subscribe;
  subscribe.packet_id = 42;
  subscribe.topic_filter = "devices/+/status";
  subscribe.requested_qos = 1;
  const std::vector<std::uint8_t> sub_encoded = cstp::EncodeSubscribePayload(subscribe);
  const cstp::SubscribePayload sub_decoded = cstp::DecodeSubscribePayload(sub_encoded);
  assert(sub_decoded.packet_id == subscribe.packet_id);
  assert(sub_decoded.topic_filter == subscribe.topic_filter);
  assert(sub_decoded.requested_qos == subscribe.requested_qos);

  cstp::SubscribeAckPayload suback;
  suback.packet_id = subscribe.packet_id;
  suback.granted_qos = 1;
  suback.message = "ok";
  const std::vector<std::uint8_t> suback_encoded = cstp::EncodeSubscribeAckPayload(suback);
  const cstp::SubscribeAckPayload suback_decoded = cstp::DecodeSubscribeAckPayload(suback_encoded);
  assert(suback_decoded.packet_id == suback.packet_id);
  assert(suback_decoded.granted_qos == suback.granted_qos);
  assert(suback_decoded.message == suback.message);

  cstp::UnsubscribePayload unsubscribe;
  unsubscribe.packet_id = 43;
  unsubscribe.topic_filter = "devices/#";
  const std::vector<std::uint8_t> unsub_encoded = cstp::EncodeUnsubscribePayload(unsubscribe);
  const cstp::UnsubscribePayload unsub_decoded = cstp::DecodeUnsubscribePayload(unsub_encoded);
  assert(unsub_decoded.packet_id == unsubscribe.packet_id);
  assert(unsub_decoded.topic_filter == unsubscribe.topic_filter);
}

}  // namespace

int main() {
  TestFrameRoundTrip();
  TestCrcMismatchRejected();
  TestDataBatchRoundTrip();
  TestBrokerPayloadRoundTrip();

  std::cout << "All CSTP protocol tests passed\n";
  return 0;
}
