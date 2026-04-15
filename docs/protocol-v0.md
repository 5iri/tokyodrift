# Custom Sensor Transport Protocol (CSTP) v0

## 1. Goal

CSTP is a custom MQTT-like protocol optimized for:

- High data volume sent at low frequency (batched sends)
- Multiple sensors in one transfer
- Simple device control commands
- Fully custom wire format and reliability behavior

This version defines the wire contract and defaults.

## 2. Baseline Limits (agreed defaults)

- max_sensors_per_batch: `64`
- max_samples_per_sensor: `200`
- send_interval_ms: `5000`
- max_frame_bytes: `1_048_576` (1 MiB)
- ack_timeout_ms: `2000`
- max_retries: `3`

## 3. Transport and Session

- Transport: `TCP`
- Security: `TLS` recommended for all non-local traffic
- Connection model: `device/gateway client -> server`
- One TCP connection can carry data stream + control stream

Rules:

1. Client sends `HELLO` immediately after TCP connect.
2. Server responds with `HELLO_ACK`.
3. Normal traffic starts only after successful `HELLO_ACK`.

## 4. Wire Encoding

- Integer byte order: **little-endian**
- Strings: `u16 length` + UTF-8 bytes
- CRC: `CRC32 (IEEE)` of all bytes from `magic` through payload (exclude trailing crc field)

## 5. Frame Format

Fixed header (23 bytes) + payload + CRC32 (4 bytes).

| Field | Size | Type | Notes |
|---|---:|---|---|
| magic | 2 | u16 | `0x4350` ("CP") |
| version | 1 | u8 | `1` for v0 |
| type | 1 | u8 | Message type enum |
| flags | 1 | u8 | Bitmask |
| msg_id | 4 | u32 | Monotonic per stream |
| stream_id | 2 | u16 | `0` control, `1` data |
| ts_ms | 8 | u64 | Sender Unix epoch ms |
| payload_len | 4 | u32 | Payload bytes |
| payload | N | bytes | Type-specific |
| crc32 | 4 | u32 | Integrity |

Constants:

- header_size: `23`
- trailer_size: `4`
- min_frame_size: `27`
- max_payload_bytes: `max_frame_bytes - 27`

## 6. Flags

- `0x01` `ACK_REQUIRED`: Receiver must reply with ACK type for this message family.
- `0x02` `IS_ACK`: Frame itself is an ACK response.
- `0x04` `COMPRESSED`: Payload compressed (v0 reserved, optional).
- `0x08` `RESERVED_1`.
- `0x10` `RESERVED_2`.
- `0x20` `RESERVED_3`.
- `0x40` `RESERVED_4`.
- `0x80` `RESERVED_5`.

## 7. Message Types

| Name | Code | Stream |
|---|---:|---:|
| HELLO | `0x01` | 0 |
| HELLO_ACK | `0x02` | 0 |
| DATA_BATCH | `0x10` | 1 |
| DATA_ACK | `0x11` | 1 |
| CMD_REQ | `0x20` | 0 |
| CMD_RESP | `0x21` | 0 |
| HEARTBEAT | `0x30` | 0 |
| ERROR | `0x7F` | 0 |

## 8. Payload Schemas (v0)

### 8.1 HELLO (`0x01`)

1. `device_id` (str)
2. `auth_token` (str)
3. `requested_keepalive_sec` (u16)
4. `client_max_frame_bytes` (u32)
5. `client_version_name` (str)

### 8.2 HELLO_ACK (`0x02`)

1. `accepted` (u8, `0`/`1`)
2. `session_id` (u32)
3. `server_time_ms` (u64)
4. `accepted_keepalive_sec` (u16)
5. `accepted_max_frame_bytes` (u32)
6. `reason` (str)

### 8.3 DATA_BATCH (`0x10`)

1. `batch_id` (u32)
2. `device_id` (str)
3. `batch_time_ms` (u64)
4. `sensor_count` (u16)
5. Repeat `sensor_count` times:
   1. `sensor_id` (u16)
   2. `metric_name` (str)
   3. `value_type` (u8)
   4. `sample_count` (u16)
   5. Repeat `sample_count` times:
      1. `delta_ms` (u32)
      2. `value` (encoded by `value_type`)

`value_type` encoding:

- `1` INT64: 8 bytes
- `2` FLOAT64: 8 bytes
- `3` BOOL: 1 byte (`0`/`1`)
- `4` BYTES: `u16 length + raw bytes`
- `5` STRING: `u16 length + UTF-8 bytes`

### 8.4 DATA_ACK (`0x11`)

1. `acked_msg_id` (u32)
2. `batch_id` (u32)
3. `status` (u8)
4. `received_at_ms` (u64)
5. `note` (str)

`status`:

- `0` accepted
- `1` duplicate_accepted
- `2` rejected

### 8.5 CMD_REQ (`0x20`)

1. `command_id` (u32)
2. `target_device_id` (str)
3. `target_sensor_id` (u16, `65535` means device-level)
4. `command_name` (str)
5. `args_json` (str)
6. `timeout_ms` (u32)

### 8.6 CMD_RESP (`0x21`)

1. `command_id` (u32)
2. `status_code` (u16)
3. `message` (str)
4. `result_json` (str)

### 8.7 HEARTBEAT (`0x30`)

1. `uptime_ms` (u64)
2. `pending_batches` (u16)
3. `last_batch_id` (u32)

### 8.8 ERROR (`0x7F`)

1. `error_code` (u16)
2. `related_msg_id` (u32)
3. `message` (str)

Error code set:

- `1001` malformed_frame
- `1002` unsupported_version
- `1003` auth_failed
- `1004` payload_too_large
- `1005` decode_failed
- `1006` unknown_message_type
- `1007` rate_limited
- `1008` internal_error

## 9. Reliability Rules (v0)

For frames with `ACK_REQUIRED`:

1. Sender starts `ack_timeout_ms` timer after send.
2. If no ACK, resend same frame (`msg_id` unchanged).
3. Retry until ACK received or `max_retries` reached.
4. On exhaustion, connection may be dropped and re-established.

Receiver dedup rule:

- Keep a short-lived cache key: `(device_id, stream_id, msg_id)`.
- If duplicate `DATA_BATCH`, return `DATA_ACK` with `duplicate_accepted`.

## 10. Validation Rules

- `sensor_count` must be `1..64`
- `sample_count` per sensor must be `1..200`
- Frame size must be `<= max_frame_bytes`
- `payload_len` must match actual payload bytes
- CRC must match

## 11. Suggested Build Sequence

1. Implement binary codec for these frame and payload types.
2. Add strict decode validation and fuzz tests.
3. Build server receive path: `HELLO -> DATA_BATCH -> DATA_ACK`.
4. Add command path: `CMD_REQ/CMD_RESP`.
5. Add persistence and replay support as needed.
