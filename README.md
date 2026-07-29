# Cortana Endpoint for ThirdReality TRSPK

Firmware for using the ThirdReality Voice & Music Assistant hardware as a
dedicated Cortana voice endpoint. This fork keeps the proven TRSPK hardware
support while replacing Sendspin, ESPHome/Home Assistant, and local wake-word
control planes with a native HTTPS/WSS Cortana connection.

The implementation sequence and acceptance gates are in
[doc/build.md](doc/build.md).

## Current architecture

The retained `linux-voice-assistant-cpp` binary name now hosts:

- protected Cortana endpoint configuration and USB provisioning;
- HTTPS device-ticket exchange and authenticated WSS sessions;
- strict protocol negotiation, keepalive, reconnect, and bounded queues;
- centralized ring LED state, home-button input, and mute GPIO handling.

There is no ESPHome listener, Home Assistant discovery, protobuf control
plane, Sendspin client, or supervisor HTTP API. Continuous ALSA capture and
hardware-loopback AEC feed generation-tagged 20 ms PCM frames over the bounded
WSS session transport. Cortana response playback follows in a subsequent plan
phase.

## Repository setup

```bash
git clone https://github.com/kaushikr7/cortana-endpoint-trspk.git
cd cortana-endpoint-trspk
git submodule update --init --recursive
```

## Build and flash

Firmware builds are intentionally deferred while the host-testable refactor is
underway. When a build gate is reached, use the supported Docker path on the
build machine:

```bash
./go --docker trspk <version>
./go --docker trspk rebuild linux-voice-assistant-cpp
```

The generated image is written under `image/` and is flashed with the Amlogic
USB Burning Tool. Burning and ADB provisioning can use the same cable, but not
at the same time: flash in Amlogic USB mode, boot normally, then wait for ADB.
See [doc/build.md](doc/build.md) for the full baseline and verification gates.

## Host-only checks

These checks do not build firmware or access a device:

```bash
./script/test_device_controls.sh
./script/test_cortana_protocol.sh
./script/test_device_ticket.sh
./script/test_cortana_session.sh
./script/test_capture_pipeline.sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s tools/tests -p 'test_*.py'
```

The fake session core has no runtime dependency. Its optional WebSocket wrapper
uses `websockets>=13,<16`:

```bash
./tools/fake_cortana_server.py
```

## Provision over USB

Do not assume `/data/cortana` survives an image burn, erase/repartition, or
factory reset. Check status after every flash and reprovision when needed.

```bash
./script/provision_cortana_endpoint.py provision \
  --serial <usb-serial> \
  --endpoint https://cortana.raintreeresearch.com \
  --satellite-id study-voice-1 \
  --expected-area-id study
```

The tool requests the credential without echo, atomically installs
`/data/cortana/config.json` and `/data/cortana/credential` with protected
permissions, validates them with the endpoint binary, and restarts the endpoint
service.

```bash
./script/provision_cortana_endpoint.py status --serial <usb-serial>
./script/provision_cortana_endpoint.py rotate-credential --serial <usb-serial>
```

When multiple ADB devices are attached, always pass `--serial`. The same
speaker exposed over USB and TCP appears as two separate ADB targets.
