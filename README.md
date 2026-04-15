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
CSTP_GPIO_MODE=mock ./build/cstp_server 18830
```

Terminal 2:

```bash
./build/cstp_client 127.0.0.1 18830
```

The client sends `HELLO`, receives `HELLO_ACK`, then sends one sample `DATA_BATCH` and expects `DATA_ACK`.

## Remote LED Command (RPi)

Server supports `CMD_REQ` with command names:

- `led_on`
- `led_off`
- `led_set` (requires value)

Start receiver on Pi (real GPIO via sysfs):

```bash
CSTP_GPIO_MODE=sysfs CSTP_LED_PIN=17 ./build/cstp_server 18830
```

From remote client, send command:

```bash
./build/cstp_client <rpi-ip> 18830 led_on 17
./build/cstp_client <rpi-ip> 18830 led_off 17
./build/cstp_client <rpi-ip> 18830 led_set 17 1
./build/cstp_client <rpi-ip> 18830 led_set 17 0
```

When a command is provided, the client sends `HELLO` then `CMD_REQ` directly.

Command response is printed as `CMD_RESP` with status code and result JSON.

## Layout

- `include/cstp/protocol.hpp`: protocol types + API
- `include/cstp/socket_io.hpp`: framed socket send/receive helpers
- `src/protocol.cpp`: binary encode/decode + CRC32 + payload codecs
- `src/socket_io.cpp`: blocking TCP frame I/O
- `tests/test_protocol.cpp`: roundtrip + CRC failure tests
- `examples/server.cpp`: minimal server skeleton
- `examples/client.cpp`: minimal client skeleton
