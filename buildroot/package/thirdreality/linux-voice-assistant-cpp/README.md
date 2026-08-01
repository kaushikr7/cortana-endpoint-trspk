# Cortana endpoint for ThirdReality TRSPK

This package builds the native Cortana endpoint while retaining the historical
`linux-voice-assistant-cpp` package and binary names so existing image and
service commands remain stable.

The current runtime provides:

- protected endpoint identity and credential loading;
- HTTPS device-ticket exchange with CA and clock validation;
- authenticated WSS negotiation, keepalive, reconnect, and bounded queues;
- one-connection replacement handling for the provisioned satellite ID;
- centralized ring LED state for boot, connection, turn, and blocked errors;
- local home-button and dedicated mute-button handling;
- continuous capture from PulseAudio's direct PDM microphone source with
  WebRTC high-pass filtering, AGC2, noise suppression, and bounded queue/timing
  metrics;
- microphone transmission isolation during playback and its short echo tail;
- exact 20 ms PCM16 microphone framing with generation, mute, reconnect, and
  backpressure discard rules.

ESPHome framing, protobuf, TCP port 6053, Home Assistant entities, mDNS
discovery, the HA satellite state machine, and the legacy supervisor/OTA HTTP
surface have been removed. Cortana response playback is connected in later
implementation tickets.

## Runtime

`S99ha-speaker` launches:

```sh
/usr/bin/linux-voice-assistant-cpp
```

Supported options:

```text
--check-config             Validate Cortana config and credential, then exit
--status                   Print redacted Cortana config status as JSON
--config-file <path>       Override /data/cortana/config.json
--credential-file <path>   Override /data/cortana/credential
--capture-source <name>     Override PulseAudio capture source
                            (default: alsa_input.hw_0_2)
--capture-mic-channel <n>  Select the interleaved microphone channel
--capture-gain-db <n>      Override AGC2 fixed gain from 0 to 49 dB
--debug                    Enable debug logging
--help                     Show help
```

The protected configuration and USB provisioning workflow are documented in
the repository [README](../../../../README.md).

## Source layout

```text
src/config/                 Protected endpoint configuration
src/cortana/                Protocol, ticket, WSS transport, session lifecycle
src/audio/                  Continuous capture, processing, and playback
src/tr/                     TRSPK LEDs, GPIO, button, and volume integration
src/tools/                  Host/device audio diagnostics
src/util/                   Logging helper
```

Buildroot dependencies currently include JSON for Modern C++, libcurl with
OpenSSL/WebSocket support, ALSA, PulseAudio, and WebRTC audio processing.
All local wake implementations, models, feature extraction, and TFLite runtime
have been removed; wake detection is owned by Cortana server-side.
