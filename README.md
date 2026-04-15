# Custom Sensor Transport Protocol (CSTP)

C++20 starter implementation for a custom MQTT-like protocol focused on:

- batched multi-sensor transfers
- low-frequency/high-volume uplink
- explicit control-command messages

Protocol spec: `docs/protocol-v0.md`

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run Tests

```bash
ctest --test-dir build --output-on-failure
```

## Run Example Handshake

Terminal 1:

```bash
./build/cstp_server 18830
```

Terminal 2:

```bash
./build/cstp_client 127.0.0.1 18830
```

The client sends `HELLO`, server replies `HELLO_ACK`.

## Layout

- `include/cstp/protocol.hpp`: protocol types + API
- `include/cstp/socket_io.hpp`: framed socket send/receive helpers
- `src/protocol.cpp`: binary encode/decode + CRC32 + payload codecs
- `src/socket_io.cpp`: blocking TCP frame I/O
- `tests/test_protocol.cpp`: roundtrip + CRC failure tests
- `examples/server.cpp`: minimal server skeleton
- `examples/client.cpp`: minimal client skeleton
