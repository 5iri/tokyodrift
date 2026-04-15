# Custom Sensor Transport Protocol (CSTP)

C++20 starter implementation for a custom MQTT-like protocol focused on:

- batched multi-sensor transfers
- low-frequency/high-volume uplink
- explicit control-command messages
- brokered topic publish/subscribe flow (`SUBSCRIBE`, `PUBLISH`, `PUB_ACK`)

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

## MQTT-like Topic Broker Flow

Start broker/server:

```bash
CSTP_GPIO_MODE=mock ./build/cstp_server 18830
```

Terminal 2 (subscriber):

```bash
./build/cstp_client 127.0.0.1 18830 sub 'demo/topic' 1
```

Terminal 3 (publisher):

```bash
./build/cstp_client 127.0.0.1 18830 pub 'demo/topic' 'hello world' 1 0
```

Notes:

- Topic filters support MQTT-style wildcards: `+` (single level), `#` (multi-level suffix).
- QoS `0` and `1` are supported in the current client/server.
- Broker currently keeps subscriptions in-memory only.

## Generic Control Plane (RPi)

Server supports a generic endpoint command namespace:

- `endpoint.register`
- `endpoint.write`
- `endpoint.list`
- `endpoint.unregister`

Start receiver on Pi (real GPIO via sysfs):

```bash
CSTP_GPIO_MODE=sysfs CSTP_PWM_BACKEND=sysfs_soft CSTP_LED_PIN=17 ./build/cstp_server 18830
```

PWM backend options:

```bash
export CSTP_PWM_BACKEND=auto       # default: try pigpio, then built-in soft PWM
export CSTP_PWM_BACKEND=pigpio     # require pigpio daemon
export CSTP_PWM_BACKEND=sysfs_soft # built-in software PWM (no pigpiod)
```

On Raspberry Pi OS `trixie`, `apt install pigpio` may be unavailable. If you want pigpio backend, build from source and run `pigpiod`; otherwise use `sysfs_soft`.

Raw command mode from client:

```bash
./build/cstp_client <rpi-ip> 18830 cmd endpoint.register '{"name":"status_led","kind":"digital_out","pin":27,"active_low":0}'
./build/cstp_client <rpi-ip> 18830 cmd endpoint.register '{"name":"pan_servo","kind":"pwm_out","pin":13,"min_pulse_us":500,"max_pulse_us":2500}'
./build/cstp_client <rpi-ip> 18830 cmd endpoint.list '{}'
./build/cstp_client <rpi-ip> 18830 cmd endpoint.write '{"name":"status_led","value":1}'
./build/cstp_client <rpi-ip> 18830 cmd endpoint.write '{"name":"pan_servo","pulse_us":1500}'
./build/cstp_client <rpi-ip> 18830 cmd endpoint.unregister '{"name":"status_led"}'
```

Direct pin control (without registry) is also available:

```bash
./build/cstp_client <rpi-ip> 18830 cmd pin.write '{"pin":17,"value":1}'
./build/cstp_client <rpi-ip> 18830 cmd pin.pwm '{"pin":13,"pulse_us":1500}'
```

When a command is provided, the client sends `HELLO` then `CMD_REQ` directly. `CMD_RESP` includes status and result JSON.

Notes:

- Endpoint registry is in-memory for now (resets on server restart).
- `pwm_out` range defaults to `500..2500`, and `pulse_us=0` stops pulses.
- For MG995-like servos, start around `pulse_us=1500`, then sweep `1000..2000`.

## Layout

- `include/cstp/protocol.hpp`: protocol types + API
- `include/cstp/socket_io.hpp`: framed socket send/receive helpers
- `src/protocol.cpp`: binary encode/decode + CRC32 + payload codecs
- `src/socket_io.cpp`: blocking TCP frame I/O
- `tests/test_protocol.cpp`: roundtrip + CRC failure tests
- `examples/server.cpp`: minimal server skeleton
- `examples/client.cpp`: minimal client skeleton
