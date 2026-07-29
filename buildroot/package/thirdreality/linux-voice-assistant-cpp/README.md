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
- continuous ALSA microphone capture with hardware-loopback AEC and bounded
  queue/timing metrics.

ESPHome framing, protobuf, TCP port 6053, Home Assistant entities, mDNS
discovery, the HA satellite state machine, and the legacy supervisor/OTA HTTP
surface have been removed. The local PCM queue is connected to WSS and playback
in later implementation tickets.

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
--capture-alsa-device <d>  Override hw:0,4
--capture-mic-channel <n>  Select the interleaved microphone channel
--capture-ref-channels <r> Select none, one, or two AEC reference channels
--debug                    Enable debug logging
--help                     Show help
```

The protected configuration and USB provisioning workflow are documented in
the repository [README](../../../../README.md).

## Source layout

```text
src/config/                 Protected endpoint configuration
src/cortana/                Protocol, ticket, WSS transport, session lifecycle
src/audio/                  Audio hardware and temporary local-wake code
src/tr/                     TRSPK LEDs, GPIO, button, and volume integration
src/tools/                  Host/device audio diagnostics
src/util/                   Logging helper
```

Buildroot dependencies currently include JSON for Modern C++, libcurl with
OpenSSL/WebSocket support, ALSA, PulseAudio, WebRTC audio processing, and MPV.
Wake and legacy media dependencies remain until their dedicated purge tickets.
