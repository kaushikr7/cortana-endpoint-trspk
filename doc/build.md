# Third Reality Cortana Voice Endpoint

## Purpose

This repository is a fork of `thirdreality/voice-music-assistant` for a
dedicated Cortana voice endpoint. This document is the complete handoff from
the Kensho workspace and the operating plan for this firmware repository.

The endpoint will:

- continuously send processed microphone PCM to Cortana over an authenticated
  WebSocket;
- rely exclusively on Cortana's server-side LiveKit wake-word detector;
- play Cortana response PCM through the built-in speaker;
- preserve useful Third Reality audio and hardware support;
- expose clear LED, mute, button, connection, and recovery behavior;
- identify itself with a unique provisioned satellite ID while accepting its
  authoritative area assignment from Cortana;
- remain small enough to understand and maintain without tracking future
  Third Reality firmware changes.

The original on-device wake-word implementation is not a target. LiveKit wake
inference was already tried on the Third Reality hardware and drove CPU usage
to approximately 90%. Do not spend time integrating or optimizing an on-device
ONNX, TFLite, MicroWakeWord, or OpenWakeWord path.

## Decision and repository boundary

Keep this firmware in a separate repository from Kensho. The Third Reality
checkout is several gigabytes before Buildroot output and contains a kernel,
U-Boot, toolchains, and a complete root filesystem. Kensho remains the source
repository for the Cortana server and its Kubernetes/Vault configuration; this
repository owns only the physical endpoint firmware and its local provisioning
tools.

This repository has already imported the upstream `linux-voice-assistant`
branch. For a clean checkout of the Cortana fork, use:

```bash
git clone --recurse-submodules \
  https://github.com/kaushikr7/cortana-endpoint-trspk.git
cd cortana-endpoint-trspk
```

Preserve the upstream Apache-2.0 attribution. There is no requirement to merge
later upstream changes. Prefer deletion and simplification when a subsystem has
no role in the final product.

When continuing in the fresh workspace, first read this file completely and
inspect the checked-out source. Do not begin by adding wake-word support or by
reintroducing Home Assistant.

## Current Cortana architecture

Cortana is the household conversation and voice-orchestration service at:

```text
https://cortana.raintreeresearch.com
```

It owns:

- device authentication and trusted satellite/area identity;
- continuous PCM ingestion and bounded central pre-roll;
- LiveKit wake detection, VAD, adjacent-room arbitration, and barge-in;
- Whisper STT, conversation/tool orchestration, Piper TTS, and response audio;
- Riven home control, Sundial calendar/task access, and room-scoped context.

The endpoint must not implement any of those server responsibilities. It owns
capture, local audio processing, framing, playback, physical controls,
connection recovery, and user-visible device status.

Cortana currently runs a single API replica because live WebSocket sessions are
process-local. A newly authenticated connection for the same satellite ID
replaces the old connection.

Raw microphone and response PCM must never be stored, logged, exposed through
diagnostics, or committed to Git. Bounded queues are permitted only in memory.

## Cortana protocol v1

### Audio format

Microphone input is exactly:

```text
encoding:       pcm_s16le
sample rate:    16000 Hz
channels:       1
frame duration: 20 ms
frame size:     320 samples / 640 bytes
```

The Third Reality ALSA capture path already produces 16 kHz signed 16-bit mono
audio in 10 ms periods after WebRTC processing. Coalesce two processed 10 ms
periods into each Cortana binary frame. Do not send the four-channel raw ALSA
capture buffer.

At 640 bytes every 20 ms, the PCM payload rate is 32,000 bytes per second before
WebSocket/TLS overhead. Never allow network backpressure to block the ALSA
capture thread. Drop bounded queued audio and reconnect on persistent overload.

### Device ticket

The device holds one unique durable credential. For every new WebSocket
connection, obtain a short-lived, one-use ticket:

```http
POST /api/v1/voice/device-ticket HTTP/1.1
Host: cortana.raintreeresearch.com
Authorization: Bearer <unique-device-credential>
Content-Type: application/json

{"satellite_id":"study-voice-1"}
```

Successful response shape:

```json
{
  "ticket": "<short-lived-signed-ticket>",
  "expiresAt": 1234567890,
  "sessionPath": "/api/v1/voice/session",
  "protocolVersion": "1",
  "satellite": {
    "satelliteId": "study-voice-1",
    "areaId": "study",
    "label": "Study voice endpoint"
  },
  "capabilities": {
    "endpointKind": "device",
    "captureMode": "continuous",
    "wakeMode": "server",
    "microphone": {
      "encoding": "pcm_s16le",
      "sampleRate": 16000,
      "channels": 1,
      "frameDurationMs": 20
    },
    "playback": true,
    "localPreRollMs": 0,
    "followUpCapture": true,
    "playbackAcknowledgements": true,
    "bargeInMode": "none"
  }
}
```

The returned satellite identity and capabilities are authoritative. Reject a
response whose satellite ID or protocol version does not match the local
configuration. If an optional expected area was provisioned, block rather than
stream when the returned area differs.

Never place the durable credential in a URL, process argument, log line,
environment dump, firmware image, or Git-tracked configuration.

### WebSocket handshake

Convert the HTTPS origin to WSS and open the returned session path:

```text
wss://cortana.raintreeresearch.com/api/v1/voice/session
```

The first two frames must be JSON text frames in this order:

```json
{
  "type": "session.authenticate",
  "protocolVersion": "1",
  "ticket": "<ticket>"
}
```

```json
{
  "type": "session.capabilities",
  "capabilities": {
    "endpointKind": "device",
    "captureMode": "continuous",
    "wakeMode": "server",
    "microphone": {
      "encoding": "pcm_s16le",
      "sampleRate": 16000,
      "channels": 1,
      "frameDurationMs": 20
    },
    "playback": true,
    "localPreRollMs": 0,
    "followUpCapture": true,
    "playbackAcknowledgements": true,
    "bargeInMode": "none"
  }
}
```

The offered capability object must exactly match the object returned by the
ticket endpoint. Cortana closes on a mismatch.

Wait for `session.ready`, validate its identity and capabilities, then send:

```json
{
  "type": "audio.start",
  "encoding": "pcm_s16le",
  "sampleRate": 16000,
  "channels": 1,
  "frameDurationMs": 20
}
```

After that event, send continuous 640-byte binary microphone frames. There is
no `wake.detected` event because wake mode is `server`, and there is no local
pre-roll because Cortana maintains server pre-roll.

### Client events

Implement these client-to-server events:

- `session.authenticate` and `session.capabilities` during setup;
- `audio.start` once continuous capture is ready;
- `session.ping` with a bounded nonce for liveness;
- `mute.changed` whenever the hardware or software mute state changes;
- `wake.manual` for a physical push-to-talk fallback;
- `turn.cancel` for the physical stop action or mute during a turn;
- `playback.started`, `playback.completed`, and `playback.stopped` for response
  delivery acknowledgement.

Do not send unknown fields. Cortana's protocol models reject extra data.

### Server events

Handle at least:

- `session.ready` and `session.health`;
- `wake.accepted` and `wake.suppressed`;
- `speech.started` and `speech.ended`;
- `transcript.final`;
- `response.started`, `response.text`, and `response.completed`;
- `audio.start` and `audio.end` for response PCM;
- `playback.stop` and `turn.cancelled`;
- structured `error` events.

Do not log transcript or response text by default on the endpoint. It is not
needed to operate the device.

### Response PCM

Cortana sends an `audio.start` event containing `turnId`, `encoding`,
`sampleRate`, `sampleWidth`, and `channels`. Subsequent binary frames are Piper
response PCM until the matching `audio.end`.

The existing Third Reality assistant expects a TTS URL and uses libmpv. Cortana
does not send a URL. Add a bounded raw-PCM player. Prefer PulseAudio playback so
the system audio path performs resampling and remains visible to the existing
hardware-loopback AEC reference. Validate every announced format and support
PCM16 only initially.

Send `playback.started` only when local playback actually starts. Send
`playback.completed` only after the local output queue drains. On physical
cancel or server `playback.stop`, clear buffered output immediately and send
`playback.stopped`.

### Liveness and reconnection

- Use verified TLS with the installed CA bundle. Never add an insecure TLS
  fallback.
- Wait for valid system time before TLS certificate validation.
- Send periodic `session.ping` messages and require the nonce to return in a
  `session.health` event.
- On disconnect, immediately stop transmitting and discard queued microphone
  PCM. Never replay stale room audio after reconnecting.
- Obtain a new device ticket for every WebSocket attempt.
- Use exponential reconnect delay with jitter and a reasonable cap, such as
  1, 2, 4, 8, 16, then 30 seconds.
- Treat authentication rejection, identity mismatch, capability mismatch, and
  invalid local configuration as blocked states rather than endless rapid
  retries.
- Record counters for frames captured, sent, dropped, response bytes, reconnects,
  and last successful ping. Never record PCM content.

## Device provisioning and trusted area identity

### Identity model

Each physical device gets a stable satellite ID such as:

```text
study-voice-1
```

The satellite ID identifies the hardware, not the room name forever. Cortana's
server-side satellite registry maps it to the authoritative Riven `area_id` and
operator label. The device must not be able to claim an arbitrary room.

Provision locally:

- Cortana endpoint URL;
- satellite ID;
- optional expected area ID used only as a commissioning safety check;
- unique device credential in a separate protected file.

Suggested target layout:

```text
/data/cortana/config.json       mode 0600, no credential
/data/cortana/credential       mode 0600, credential only
/data/cortana/state.json       mode 0600, non-secret bounded runtime state
```

Suggested `config.json`:

```json
{
  "schemaVersion": 1,
  "endpoint": "https://cortana.raintreeresearch.com",
  "satelliteId": "study-voice-1",
  "expectedAreaId": "study"
}
```

The endpoint should fail closed when configuration permissions are too broad,
the credential is missing, or the returned trusted identity differs from the
configured satellite/optional expected area.

### Provisioning command

Add a small provisioning tool or subcommand rather than editing JSON manually.
It should:

- accept the credential only on standard input or through an interactive
  no-echo prompt;
- validate endpoint, satellite, and expected-area syntax;
- write files atomically with mode `0600` under `/data/cortana`;
- never print the credential;
- offer a read-only status command that redacts secrets;
- support replacing the credential during rotation.

T1.2 implements host tools as `script/provision_cortana_endpoint.py` for Linux
and `script/provision_cortana_endpoint.ps1` for native Windows PowerShell. The
device-side `linux-voice-assistant-cpp` binary exposes `--check-config` and
`--status`. As of T2.4, normal startup requires this configuration and either
provisioning tool restarts the endpoint after a validated install or credential
rotation.

USB ADB or authenticated SSH is sufficient for initial provisioning. Do not
build a web or BLE commissioning interface for the first endpoint.

Wi-Fi commissioning uses `script/provision_wifi.py` on Linux or
`script/provision_wifi.ps1` on native Windows. Both use protected temporary
files and device-side file expansion so the PSK is never placed in host history
or the host ADB argument list. The existing device `wifi_connect` process sees
the PSK only for the duration of the USB commissioning operation.

USB firmware burning and USB ADB provisioning are sequential modes, not
competing services. Flash first in Amlogic burn mode, then boot normally and
provision over ADB. Although the current full-image manifest does not include a
`data` payload, never rely on `/data/cortana` surviving a full burn,
erase/repartition option, data-volume migration, or factory reset. Check
redacted status after every flash and reprovision when necessary.

### Server-side provisioning in Kensho

These changes belong in Kensho, not in this firmware repository:

1. Add an explicit Cortana satellite entry with the stable satellite ID, area,
   label, enabled state, and complete capability object.
2. Generate at least 32 random bytes for the unique device credential and a
   separate device-ticket signing secret.
3. Store server-side material in Vault.
4. Expose it to Cortana as `CORTANA_DEVICE_CREDENTIALS` and
   `CORTANA_DEVICE_TICKET_SECRET` through External Secrets.
5. Provision only that device's credential onto the device.
6. Permit at most two credentials temporarily during rotation.
7. Revoke a device by disabling/removing its registry entry and credential.

The explicit server capability profile for the first device should be:

```json
{
  "endpointKind": "device",
  "captureMode": "continuous",
  "wakeMode": "server",
  "microphone": {
    "encoding": "pcm_s16le",
    "sampleRate": 16000,
    "channels": 1,
    "frameDurationMs": 20
  },
  "playback": true,
  "localPreRollMs": 0,
  "followUpCapture": true,
  "playbackAcknowledgements": true,
  "bargeInMode": "none"
}
```

Start with `bargeInMode=none`. After hardware-loopback AEC is proven during
response playback, change it to `full_duplex` and validate server wake-word
interruption. A continuous endpoint does not use the satellite `wake_word`
barge-in mode.

If the device can hear the same room as a Halo tablet or another endpoint, add
the satellite IDs to the appropriate Cortana acoustic overlap group so one wake
candidate wins deterministically.

## LED and physical-control contract

### Design rules

- One component must own durable ring state. Do not let the voice endpoint,
  volume scripts, and update code continuously fight over the ring.
- Use a small priority-based `LedController` rather than direct LED calls spread
  through the state machine.
- The physical mute button has its own LED. That dedicated LED is the sole mute
  indicator; do not consume the ring or add a ring animation for mute/unmute.
- Ready/armed should be dark after a short success indication. A permanently
  glowing ring is distracting.
- Transient network loss should not flash an error immediately. Show a fault
  only after a short grace period.
- Blocked configuration/authentication states must remain visible on the ring.
- Verify the dedicated mute-button LED remains synchronized with the physical
  mute state independently of Cortana connectivity and ring activity.
- Reuse the existing D-Bus LED service and animation-file format. Create clearer
  Cortana-specific animations when the existing names/colors are ambiguous.

Suggested priority from highest to lowest:

1. blocked configuration/authentication;
2. firmware update or unrecoverable device fault;
3. active Cortana turn;
4. degraded/reconnecting connection;
5. short volume or button overlay;
6. ready/armed idle.

Suggested visible states:

| Endpoint state | LED behavior |
| --- | --- |
| booting / initial connection | blue rotating or breathing animation |
| ready and armed | brief cyan success sweep, then off |
| reconnecting for more than 3 seconds | slow amber breathing |
| blocked config, credential, identity, or capability | persistent red pattern |
| wake accepted / hearing | existing `active-waking.animation` |
| transcribing / thinking | existing `active-thinking.animation` |
| speaking | existing `active-talking.animation` |
| follow-up listening | existing `active-waking.animation` |
| recoverable protocol error | one short `error.animation`, then current state |
| firmware update | distinct purple progress/rotation pattern |

T1.3 replaces the legacy C++ `LedRing` with `LedController`; protocol errors now
use `error.animation` rather than the previous incorrect
`active-ending.animation` mapping.

Remove Sendspin's LED behavior with the rest of Sendspin. Route short volume
feedback through the shared controller so it cannot overwrite a Cortana turn or
fault state.

### Home button

Refactor `HomeButton` so it emits a local callback and no longer depends on
ESPHome protobuf entities or `ServerState.broadcast`.

Initial behavior:

- single press while armed: send `wake.manual` with a fresh activation ID;
- single press during an active turn or playback: stop local playback and send
  `turn.cancel`/`playback.stopped` as appropriate;
- multiple-click actions: leave unassigned until a real interaction is chosen.

Do not hide provisioning or destructive reset behind an undocumented
multi-click gesture.

### Mute

Hardware mute always wins. When muted:

- stop sending microphone frames immediately;
- discard queued microphone audio;
- cancel any active turn with source `mute`;
- send `mute.changed` if connected;
- ensure the dedicated mute-button LED indicates the hardware state;
- do not change the ring merely because mute was engaged or released;
- do not reconnect merely because mute is active.

On unmute, send `mute.changed`, resume continuous capture with a fresh bounded
queue, and return to the appropriate connection state.

## Code strategy

### Keep

Retain or extract these proven hardware-oriented parts:

- ALSA `AudioCapture` at 16 kHz with four-channel input splitting;
- `WebRtcProcessor` AGC, noise suppression, and hardware-loopback AEC;
- `PcmRingBuffer`, revised to expose bounded/drop metrics cleanly;
- microphone mute GPIO handling;
- home-button Linux input handling after removing HA coupling;
- LED D-Bus transport and animation assets;
- system volume handling;
- SWUpdate/image packaging required to deploy the device;
- minimal Wi-Fi, NTP, CA certificates, ADB-over-USB, and recovery support.

### Remove in dependency-safe slices

Purge the old product rather than carrying dormant integrations indefinitely,
but do it after the baseline image and at the point each replacement becomes
testable:

1. Remove Sendspin immediately after the baseline. Delete its Buildroot
   selection and package, Avahi service, configuration/persistence, supervisor
   branches, signal bridge, LED writer, and documentation. Nothing in the
   Cortana endpoint needs it.
2. After the Cortana ticket/WebSocket control plane is usable, remove ESPHome
   framing, protobufs, TCP server, mDNS publisher, entities, `ServerState`, the
   HA `Satellite` state machine, timezone sync, HA OTA/entity wrappers, and the
   legacy supervisor HTTP API. This should also remove target and host protobuf
   plus Avahi if no remaining rootfs consumer selects it.
3. After continuous PCM ingress works, remove every local wake engine, scanner,
   feature extractor, model, sensitivity preference, vendored TFLite library,
   microfrontend source, wake debug tool, and model install hook. Keep
   `aec_loopback_test` as a development diagnostic until AEC acceptance.
4. After raw PCM Cortana playback works, remove `LibMpvPlayer`, MPV, and FFmpeg
   if Buildroot's dependency graph shows no other selected consumer. Remove
   unused timer/thinking/wake sounds; retain only sounds actually used for mute,
   setup, error, or recovery behavior.
5. During final image cleanup, delete the unselected Python assistant and its
   wake-only packages, trim ALSA plugins/utilities, and remove command-line
   `curl`, OpenSSL, or `jq` only after scripts and SWUpdate dependencies have
   been checked. Shared libcurl/OpenSSL, PulseAudio, ALSA, WebRTC APM, NTP, the
   CA bundle, Wi-Fi, SWUpdate, and USB recovery remain product requirements.

Do not remove Bluetooth, hostapd, dnsmasq, or the existing network monitor until
the first-device Wi-Fi onboarding path has been exercised. They may be part of
the factory setup flow even though Cortana itself does not use them.

For every dependency slice, use Buildroot's dependency information and `rg`
before deletion, then perform both an incremental package build and a full image
build. Directory presence under `buildroot/package/thirdreality` does not mean a
package is selected; distinguish repository cleanup from image-size cleanup.

### Suggested focused layout

The final application and Buildroot package should be called
`cortana-endpoint-trspk` so it is unambiguously tied to this hardware, and
should trend toward this structure:

```text
src/main.cpp
src/config/EndpointConfig.{h,cpp}
src/cortana/DeviceTicketClient.{h,cpp}
src/cortana/SessionClient.{h,cpp}
src/cortana/Protocol.{h,cpp}
src/cortana/EndpointState.{h,cpp}
src/audio/AudioCapture.{h,cpp}
src/audio/WebRtcProcessor.{h,cpp}
src/audio/PcmRingBuffer.{h,cpp}
src/audio/PcmPlayback.{h,cpp}
src/device/HomeButton.{h,cpp}
src/device/LedController.{h,cpp}
src/device/MicMute.{h,cpp}
src/device/SystemVolume.{h,cpp}
src/util/Log.{h,cpp}
```

Use one endpoint state machine as the source of truth for network, turn,
playback, mute, and LED state. Avoid callbacks that independently infer state.

### Networking implementation

The current Buildroot contains libcurl 8.7.1, OpenSSL, nlohmann JSON, and code
that already references the system CA bundle. Its libcurl package supports
WebSockets, but `3reality_trspk_defconfig` does not currently select
`BR2_PACKAGE_LIBCURL_WEBSOCKETS_SUPPORT`; without that selection Buildroot
passes `--disable-websockets`. Enable it and prove `curl_ws_send`/`curl_ws_recv`
on the target before building the session client around libcurl. If that check
fails, choose a different small TLS WebSocket client in T2.2 rather than
spreading a workaround across the endpoint.

Use a dedicated network thread or event loop with bounded thread-safe queues.
The ALSA capture callback must never perform DNS, TLS, HTTP, JSON, or WebSocket
work. Preserve WebSocket ordering between control events and binary frames.

### Playback

This is a voice-only endpoint. Remove Sendspin rather than adapting or
supervising it. Cortana TTS and short local device sounds are the only required
application playback paths.

Use the same physical output path for TTS and the AEC reference. Validate that
PulseAudio playback appears in ALSA reference channels 2 and 3. Do not advertise
full-duplex barge-in until measured playback echo is sufficiently cancelled.

## Build and development workflow

### Fix Docker context first

The upstream Dockerfile copies no repository files because `/build` is mounted
at runtime, but the upstream repository currently has no `.dockerignore`.
Create:

```dockerignore
*
!Dockerfile
```

This prevents Docker from sending the multi-gigabyte source and Buildroot
output as image-build context.

### Cold build

The Docker path removes the Ubuntu 20.04 VM requirement but does not make the
first Buildroot build fast. These commands only need to run on the dedicated
Docker-capable builder; an editing workspace does not need Docker, the toolchain
submodule, Buildroot downloads, or an output tree:

```bash
git submodule update --init --depth 1
df -h .
du -sh . buildroot/dl output image 2>/dev/null || true
./go --docker trspk baseline
```

The container bind-mounts the repository, so `output/3reality_trspk` and
Buildroot downloads survive the disposable container. Preserve that output
between iterations.

Before changing code, complete one baseline build and retain its `.img` and
`.swu` artifacts as recovery images. If possible, boot that image and record:

- ALSA capture/playback device names and formats;
- PulseAudio source/sink names;
- microphone and AEC reference channel behavior;
- LED, mute, button, ADB, SSH, NTP, and Wi-Fi behavior;
- CPU and memory at idle, during capture, and during playback.

### Package rebuild

For application-only edits:

```bash
./go --docker trspk rebuild linux-voice-assistant-cpp
```

During the transition, keep the package name until renaming it is worthwhile.
Buildroot's local-site package flow re-syncs edited source and performs an
incremental CMake build. A later full command reuses the output tree and
regenerates the root filesystem/image without rebuilding unchanged packages:

```bash
./go --docker trspk cortana-dev
```

`rebuild` validates the application package; it is not a substitute for a full
image build after a defconfig, dependency, init-script, or rootfs change. Use a
distinct version label for release artifacts because repeated builds with the
same label overwrite `image/trspk_<version>.img` and `.swu`.

### Fast hardware loop

Avoid creating and flashing an image for every C++ edit. After a package
rebuild, stage the binary from:

```text
output/3reality_trspk/target/usr/bin/linux-voice-assistant-cpp
```

Push it to a non-production path under `/data`, stop the packaged service, and
run it manually over ADB or SSH. Do not overwrite the recovery binary until the
new process has passed capture, playback, reconnect, and mute checks.

A full image is required when changing Buildroot configuration, shared library
dependencies, init scripts, rootfs assets, security defaults, or update
packaging.

## Security improvements

The upstream development image enables unauthenticated root ADB over TCP port
5555 and documents a default root SSH password. These are unacceptable for a
permanently installed microphone.

Before production acceptance:

- disable ADB over TCP while retaining USB ADB for recovery;
- remove the default SSH password and either disable SSH or require a unique
  key provisioned outside Git;
- bind any local diagnostics interface to localhost or remove it;
- verify all HTTPS/WSS certificates against the system CA bundle;
- ensure configuration and credentials are mode `0600` under `/data`;
- never log credentials, tickets, Authorization headers, PCM, transcripts, or
  response text;
- use SHA-256 and signed SWUpdate artifacts rather than treating MD5 as an
  authenticity check;
- run the endpoint under a watchdog with bounded restart/backoff behavior;
- retain a documented USB recovery procedure and known-good image.

## Implementation sequence and Sol thinking depth

Use the depth on the individual ticket, not a blanket setting for the whole
project:

- **Low**: inventory, mechanical build changes, documentation, or repeatable
  validation with an already-defined result.
- **Medium**: the default for a contained design/code change with known hardware
  and protocol boundaries.
- **High**: only for concurrent audio/network state, bounded backpressure,
  cancellation, or real-time playback correctness.
- **Extra-high**: not assigned to a planned ticket. Escalate only for an
  unresolved cross-layer race, ABI/toolchain fault, kernel/audio-driver issue,
  or an AEC problem that remains after measurements at High.

The ticket boundaries are deliberate. Finish each gate before combining later
cleanup so a regression can be assigned to one dependency slice.

### Phase 0: known-good baseline — overall Medium

#### T0.1 Docker and source setup — Low

- Add `.dockerignore` with only the Dockerfile included.
- On the Docker-capable builder, initialize submodules and record disk usage
  before the cold build. Build-only submodules are optional in editing-only
  workspaces.
- On that builder, run `./go --docker trspk baseline`; do not treat the long
  first Buildroot compile as evidence that incremental builds will be equally
  slow.

#### T0.2 Boot and hardware inventory — Medium

- Preserve the baseline `.img` and `.swu`, then boot the image.
- Record ALSA/Pulse names and formats, four-channel capture/AEC behavior, LED,
  mute, home button, Wi-Fi onboarding, Bluetooth involvement, NTP, ADB, SSH,
  CPU, memory, and recovery behavior.
- Confirm the existing firmware can be restored before modifying flash again.

#### T0.3 Incremental development loop — Low

- Change a harmless version/log string, run the package-only rebuild, stage the
  binary under `/data`, and run it manually over USB ADB or SSH.
- Confirm which rootfs/image changes require a full build and record exact
  device stop/start/copy commands.

Gate: a recoverable baseline image, measured audio facts, and a proven fast
application loop exist.

### Phase 1: remove Sendspin and isolate device services — overall Medium

#### T1.1 Complete Sendspin removal — Medium

- Remove `BR2_PACKAGE_SENDSPIN_CLIENT`, its package and `Config.in` entry,
  Avahi service, init-script paths, persistence, C++ `SendspinSignal`, LED
  behavior, documentation, and stale AEC comments.
- Keep PulseAudio and prove capture, local playback, LEDs, buttons, mute, and
  Wi-Fi still work in a full image.

#### T1.2 Endpoint configuration and USB provisioning — Medium

- Add strict config/credential parsing, atomic writes, `0600` permissions,
  redacted `--check-config`, and a read-only status command.
- Provision the stable satellite ID, optional expected area ID, Cortana URL,
  and credential through USB. Invalid or incomplete configuration must fail
  closed with a useful LED/status reason.

#### T1.3 One device state machine — Medium

- Refactor home button and mute GPIO away from protobuf/`ServerState`.
- Replace competing LED writers with `LedController` priority rules, including
  correct use of `error.animation`.
- Use small deterministic tests for parsing, LED priority, mute, manual wake,
  and cancel transitions; do not add broad snapshot-style tests.

Implementation note: the selected C++ application owns application ring state.
Boot, factory-reset, and Wi-Fi-commissioning scripts retain only their explicit
lifecycle animations. Volume scripts no longer write directly to the ring;
their preference update is observed by the application controller.

Gate: Sendspin is absent, the device boots normally, and configuration, mute,
button, and LED behavior no longer require Home Assistant state.

### Phase 2: Cortana control plane and ESPHome retirement — overall High

#### T2.1 Protocol types and fake server — Medium

- Implement strict JSON/event models and a protocol-faithful host fake server.
- Cover valid handshake, capability mismatch, malformed/unknown control events,
  authentication rejection, and playback event ordering.

Implementation paths are `src/cortana/Protocol.{h,cpp}` and
`tools/fake_cortana_server.py`. The fake session core is dependency-free; its
optional WebSocket wrapper uses a disposable host installation of
`websockets>=13,<16`.

#### T2.2 Ticket client and TLS — Medium

- Implement ticket POST, expiry handling, credential redaction, CA validation,
  and clock-not-ready handling with libcurl.
- Select `BR2_PACKAGE_LIBCURL_WEBSOCKETS_SUPPORT` and prove the target build
  exposes the required WebSocket API. If it does not, change the library choice
  here rather than adding a compatibility shim throughout the endpoint.

Implementation paths are `src/cortana/DeviceTicket.{h,cpp}` and
`DeviceTicketClient.{h,cpp}`. The Buildroot package now forces OpenSSL, the CA
bundle, and libcurl WebSocket support. `LibcurlSupportsWebSockets()` provides
the runtime target check for the later session client; executing that check is
deferred until the first planned firmware build.

#### T2.3 WebSocket session lifecycle — High

- Implement authenticated WSS, capability negotiation, ping, reconnect,
  ticket refresh, replacement close, bounded queues, and jittered backoff.
- Keep network work off audio callbacks and define single-owner rules for
  connection and endpoint state.

Implementation paths are `src/cortana/SessionClient.{h,cpp}`,
`SessionTransport.h`, and `CurlSessionTransport.{h,cpp}`. One worker owns the
ticket exchange, libcurl handle, handshake, connection state, ping, and queue
draining. Other threads can only enqueue bounded text commands or consume
generation-tagged events; neither operation performs network I/O. Every
connection attempt obtains a fresh single-use ticket. Transient failures use
bounded equal-jitter exponential backoff, while close code `4001` (connection
replaced) and authentication/capability rejection fail closed to prevent two
endpoints with the same satellite ID from evicting each other indefinitely.
The host suite covers handshake ordering, negotiated identity validation,
keepalive, reconnect with a fresh ticket, replacement close, queue bounds, and
deterministic backoff. Target libcurl/runtime WSS validation remains deferred
to the first planned firmware build and device-test phase.

#### T2.4 Remove ESPHome/HA control-plane code — Medium

- Remove protocol framing, protobuf generation/dependencies, API server, mDNS,
  entities, `ServerState`, HA `Satellite`, timezone sync, HA updater wrappers,
  supervisor HTTP API, and their CMake sources.
- Remove Avahi only if no selected rootfs package still needs it. Retain the old
  Buildroot package/binary name temporarily to keep rebuild commands stable.

Gate: the endpoint authenticates and remains connected to the fake server with
no ESPHome listener, protobuf, HA state machine, or Home Assistant discovery.

Implementation note: `main.cpp` now loads the protected Cortana configuration,
starts `SessionClient`, drains its bounded event queue, maps session state to
the shared LED controller, and polls only the local mute and home-button
controls. The ESPHome protocol/protobuf tree, entities, HA satellite,
`ServerState`, mDNS publisher, timezone bridge, updater wrappers, supervisor
HTTP API, host/target protobuf dependencies, and Avahi selection were deleted.
The historical Buildroot package, executable, init script, and service command
names remain temporarily stable. USB provisioning restarts that service after
validated configuration or credential changes.

### Phase 3: continuous microphone ingress and wake purge — overall High

#### T3.1 Capture/AEC extraction — Medium

- Reuse ALSA capture, channel splitting, `WebRtcProcessor`, and the PCM ring
  buffer while removing their dependency on local wake scanners.
- Expose bounded queue, overrun, AEC-reference, and timing metrics.

Implementation paths are `src/audio/CapturePipeline.{h,cpp}`,
`CaptureFrame.{h,cpp}`, `AudioCapture.{h,cpp}`, and `PcmRingBuffer.{h,cpp}`.
The production path is ALSA-only: each exact 10 ms four-channel period is
split into mono microphone and loopback reference, processed through WebRTC,
and written to one bounded SPSC queue. Metrics expose queue capacity/current
depth/high-water mark, written/read/dropped/discarded samples, successful AEC
reference periods, ALSA recoveries and short reads, processing failures, last
period time, and worst processing duration. The main binary deliberately drains
the queue until T3.2 supplies generation-aware WSS transport, preventing stale
latency without coupling capture to any local wake scanner.

#### T3.2 Real-time PCM transport — High

- Coalesce exactly two 10 ms periods into each 640-byte frame.
- Send only after `session.ready` and `audio.start`; discard audio across mute,
  disconnect, reconnect, or overload rather than accumulating latency.
- Test queue bounds, ordering, reconnect generation changes, and backpressure.

Implementation paths are `src/audio/MicrophoneIngress.{h,cpp}` and the bounded
audio additions to `cortana/SessionClient`. After `session.ready`, the network
owner sends `audio.start` before exposing `audio_started`; only then can the
main-loop ingress consume capture data. It reads exactly two 160-sample ALSA
periods, packs 320 PCM16 samples as 640 explicit little-endian bytes, and tags
the frame with the active session generation. The session worker is the only
code that performs binary WSS sends. Capture and session queues are both
bounded; mute, non-ready state, generation change, send failure, or queue
pressure discards pending audio instead of carrying latency across state
boundaries. Host tests cover byte ordering, exact framing, handshake ordering,
mute, bounds, scripted backpressure, and reconnect isolation.

#### T3.3 Real-device ingress acceptance — Medium

- Verify 16 kHz mono PCM16, frame rate, bytes per second, dropped frames, AEC
  reference, CPU, memory, mute, Wi-Fi loss, and reconnect on the speaker.
- If Wi-Fi reassociates without a usable IPv4 default route, the retained
  network monitor must restart DHCP with a cooldown and allow Cortana to obtain
  a fresh ticket without a device reboot.
- Record representative speech at realistic near- and far-field distances and
  inspect PCM RMS, peaks, clipping, and server recognition reliability. Confirm
  the WebRTC digital gain provides adequate microphone sensitivity; tune or
  expose it as configuration based on measurements rather than relying on the
  existing `sound.json`/`amixer` microphone-gain control, which is not yet
  proven effective on this hardware.

Device measurements found channel 0 louder than channel 1 but still only about
19 PCM RMS after AEC at less than one metre, versus Cortana's 655 PCM RMS VAD
threshold. Production capture therefore uses AGC2 fixed digital gain at 42 dB
with its limiter, replacing AGC1's 31 dB ceiling. The gain can be overridden
for foreground acceptance with `--capture-gain-db 0..49`; the AEC diagnostic
uses the same 42 dB default and accepts `--gain-db 0..49`. Recheck near- and
three-metre RMS, clipping, wake/VAD/STT reliability, and AEC with the next image.

#### T3.4 Remove all local wake code/assets — Medium

- Delete MicroWakeWord/OpenWakeWord/external wake code, TFLite runtime and
  library, microfrontend, models, install hooks, wake preferences, debug tools,
  and wake-only packages. Retain only the AEC diagnostic.
- Full-build and confirm no wake model or TFLite library remains in the target
  filesystem and idle/capture CPU have not regressed.

Implementation removes the unused legacy Python assistant and its wake-only
Buildroot packages, both native local-wake implementations, all bundled wake
models, the TFLite C runtime, microfrontend sources, wake preferences, install
hooks, and wake debug tools. Server-side wake protocol messages and the manual
home-button fallback remain because neither performs inference on the device.
The ALSA/WebRTC `aec_loopback_test` diagnostic is retained. Run
`script/test_no_local_wake.sh` for the host-side absence check; target-filesystem
and CPU confirmation remain part of the next full image/device acceptance run.

Gate: bounded continuous audio reaches the fake server and the image contains
no on-device wake implementation or model.

### Phase 4: response playback and media-stack purge — overall High

#### T4.1 Bounded raw PCM player — High

- Implement PulseAudio PCM playback from announced format metadata with a
  bounded queue, exact drain semantics, immediate flush/stop, and errors that
  cannot deadlock the session thread.
- Confirm playback uses the physical output seen by the AEC reference channels.

Implementation uses a dedicated worker with a hard 256 KiB ceiling and a
format-aware 500 ms application queue for announced PCM16LE mono/stereo
formats. WebSocket binary chunks are limited to 64 KiB and accepted only
between matching `audio.start`/`audio.end` events. Writes are split into at
most 20 ms slices so cancellation cannot sit behind a large received chunk.
Completion is emitted only after PulseAudio reports an exact drain; stop,
mute, disconnect, and cancellation discard the application queue and
interrupt an in-progress drain before flushing. PulseAudio's device buffer is
limited to about 100 ms. Production playback explicitly selects
`alsa_output.hw_0_1`, which `/etc/pulse/default.pa` maps to ALSA `hw:0,1`; the
AEC diagnostic documents and measures that output on capture channels 2/3.
Run `script/test_pcm_playback.sh` for bounded queue, drain, interrupt, flush,
format, and error-path coverage. Physical playback/AEC confirmation remains
part of the next full image acceptance run.

#### T4.2 Turn control, acknowledgements, and LEDs — High

- Implement playback started/completed/stopped acknowledgements after the
  correct physical drain or flush point.
- Map listening, thinking, speaking, cancellation, reconnect, and blocked state
  to the endpoint state machine and ring priority. Keep mute indication on the
  dedicated mute-button LED.

Implementation adds one `EndpointState` reducer for session generation,
server turn events, physical playback results, mute, and the active turn ID.
The home button and mute policy cancel that exact turn when no response is
playing. During playback they follow Halo's established ordering:
`playback.stopped` after physical flush, followed by `mute.changed` when mute
initiated the stop; Cortana then reports the turn cancellation.
`playback.started` is queued only after the first successful PulseAudio write,
`playback.completed` only after the exact drain result, and
`playback.stopped` only after flush (or a flushed playback error). Results from
a disconnected generation are logged but never acknowledged on a replacement
session. A bounded generation-tagged queue preserves acknowledgement ordering
across temporary command-queue pressure without carrying it across reconnect.
`EndpointLedPolicy` maps listening, thinking, speaking, cancellation,
reconnect, and blocked state through the controller's priority layers.
Cancellation uses `active-ending.animation`; mute has no ring state because the
hardware button owns its own mute LED. Host coverage is in
`script/test_endpoint_state.sh`, `script/test_pcm_playback.sh`,
`script/test_device_controls.sh`, and `script/test_cortana_protocol.sh`.

#### T4.3 Remove MPV/FFmpeg and trim sound assets — Medium

- Delete `LibMpvPlayer` and remove MPV/FFmpeg selections only after Buildroot's
  graph confirms there is no remaining consumer.
- Remove unused sounds while retaining only feedback that has a named runtime
  owner and exercised call path.
- Constrain announced response PCM to 8-48 kHz PCM16 mono/stereo, matching
  Halo's accepted range and the TRSPK's 48 kHz PulseAudio hardware sink. Reject
  larger formats instead of spending memory and CPU on an unused capability.
- Run dependency-graph, host-protocol, player, and absence checks here. Defer
  the expensive full image build until T4.4 so one build validates both tickets.

Implementation removes the unused libmpv adapter/interface, native endpoint
MPV dependency, explicit MPV/FFmpeg defconfig selections, four unreferenced
sound assets, and their target install hook. PulseAudio remains selected for
bounded raw PCM. Both protocol parsing and playback reject response formats
above 48 kHz. Run `script/test_no_legacy_media.sh` for the static dependency,
asset, and format-ceiling guard; target graph/rootfs confirmation remains part
of the combined T4.3/T4.4 image build.

#### T4.4 Extract the endpoint runtime coordinator — Medium

- Follow Halo's controller/transport/player boundary by moving server-event
  dispatch, playback-result handling, generation-bound acknowledgement retry,
  cancellation ordering, and mute/home decisions out of `main.cpp` into one
  `EndpointRuntime` coordinator.
- Keep `main.cpp` responsible only for construction, hardware polling, shutdown,
  and periodic metrics. Keep `SessionClient` as the sole network owner,
  `RawPcmPlayer` as the sole playback owner, and `EndpointState` as the pure
  state reducer; do not merge their threads or duplicate their state.
- Inject the session-command and playback boundaries so runtime behavior is
  covered with deterministic host tests, including stale generations, command
  pressure, physical cancel, mute during playback, disconnect, and exact
  acknowledgement ordering.
- Preserve behavior rather than redesigning the protocol. After the host suite
  passes, run one full image build covering T4.3 and T4.4, then validate TTS,
  local feedback, AEC reference, image size, boot time, and runtime metrics.

Implementation moves server-event playback dispatch, physical mute/home
policy, session-generation isolation, mute synchronization, and bounded ordered
control/acknowledgement retry into `cortana/EndpointRuntime.{h,cpp}`. The
runtime is single-threaded and injected with session, capture, playback, and
activation-ID functions; it does not take ownership away from the existing
network, ALSA, or PulseAudio workers. `main.cpp` now constructs those owners,
polls hardware, forwards events/results, renders the reduced state, and logs
metrics. Run `script/test_endpoint_runtime.sh` for playback ordering, physical
cancel, mute ordering, command pressure, activation, and stale-generation
coverage. Recoverable `no_speech`/`turn_unknown` events and authoritative
idle/armed health now release abandoned input turns, so the home button cannot
mistake a completed server turn for a local cancellation target. The combined
T4.3/T4.4 target build remains deferred as planned.

Gate: Cortana PCM plays completely, stops immediately when cancelled, and the
production image no longer carries a general media player stack. The endpoint
runtime has one tested coordinator rather than turn/control policy embedded in
the hardware wiring loop.

### Phase 5: Cortana/Kensho activation — overall Medium

#### T5.1 Cross-endpoint protocol conformance — Medium

- Create a versioned, secret-free Cortana voice v1 conformance corpus covering
  ticket capabilities, handshake, continuous microphone framing, response PCM,
  playback acknowledgement ordering, mute, manual activation, cancellation,
  reconnect generations, strict unknown-field rejection, and terminal errors.
- Run the same fixtures against Cortana's server models, Halo's transport, and
  the TRSPK protocol/runtime tests. Document the explicit export/update step
  between the private Kensho repo and this public firmware repo; fixtures must
  contain no credentials, real satellite IDs, transcripts, or PCM recordings.
- Treat fixture version/checksum drift as a failing test rather than adding
  endpoint-specific compatibility behavior.

Implementation note: the canonical corpus lives in the private Kensho repo at
`cortana/testdata/voice_protocol_v1_conformance.json`. Its byte-for-byte public
export is `testdata/voice_protocol_v1_conformance.json` in this repo. To update
it, first change and validate the canonical fixture against Cortana and Halo,
then replace the public copy, update the pinned SHA-256 in all three consumers,
and run `./script/test_protocol_conformance.sh`. Never export credentials, real
satellite or area identifiers, transcripts, or recorded PCM.

#### T5.2 Capture lifecycle supervision — Medium

- Follow Halo's microphone-controller lifecycle: distinguish capture starting,
  ready, degraded, and blocked state; detect an exited/stalled ALSA capture
  worker; and retry with bounded backoff without rebooting the speaker.
- Flush capture and session audio at every recovery boundary so restarted
  capture cannot replay stale PCM. Keep hardware/configuration failures visible
  in status, metrics, and LED priority, and avoid a tight restart loop.
- Add deterministic host tests for retry scheduling and state transitions, then
  force a capture failure on the real device during T5.4.

Implementation note: `CaptureSupervisor` now owns starting, ready, degraded,
and blocked transitions for the ALSA worker. Exits and two-second stalls cross
a recovery boundary that stops capture, clears capture and session PCM, resets
AEC state, and retries with capped exponential backoff. Periodic diagnostics
include lifecycle/failure/retry counters, and the ring shows reconnecting or
error above turn activity while capture is degraded or blocked. A physical
unmute performs an immediate planned restart with fresh queues and AEC state,
avoiding the measured delayed ALSA stall and recovery flash. The forced ALSA
failure and recovery remains a T5.4 device acceptance check.

A subsequent muted soak proved the underlying Amlogic capture path can also
stall while completely idle, independent of mute transitions. Production now
opens a dedicated 48 kHz stereo silent PulseAudio stream before starting ALSA
capture and keeps it open for the process lifetime so the playback DMA feeding
the codec loopback remains active. The stream retries independently if
PulseAudio fails, and periodic diagnostics expose its ready, stream, restart,
and error counters. T5.4 must include a long muted soak with
`capture_stalls=0`, `keepalive_ready=1`, and `keepalive_errors=0`, followed by
real response playback to confirm that the silent stream neither suppresses
TTS nor breaks the AEC reference channels.

#### T5.3 Server-side device registration — Medium

- Add the satellite with explicit continuous/server-wake capability, trusted
  area mapping, device authentication material, and acoustic overlap settings.
- Add Vault/ExternalSecret material without putting credentials in either repo.

Implementation note: Kensho registers `study-cortana-trspk` as an enabled
device endpoint with continuous capture, server wake, `bargeInMode=none`, and
trusted `area_id=study`. Its credential and device-ticket key are sourced only
through the `cortana-device-auth` ExternalSecret. The Study Halo and TRSPK
registrations share an area, so they already compete in the same arbitration
window; cross-room acoustic overlap remains empty until physical calibration
identifies an adjacent area that must join the group. A production contract
test pins the registration, capability corpus, and secret-reference wiring.

#### T5.4 Provision and exercise the physical endpoint — Medium

- Provision through USB and verify ticket, trusted area, server wake, VAD, STT,
  Riven, Sundial, Piper, playback, mute, manual activation, cancellation, LED
  errors, and connection recovery.
- Force capture-worker failure/recovery and confirm the endpoint returns to
  fresh continuous PCM without a device reboot or stale buffered audio.
- Keep `bargeInMode=none`; this phase does not depend on full-duplex acoustics.

Gate: representative voice turns work end to end in the correct area without
Home Assistant, Sendspin, or on-device wake.

### Phase 6: final product cleanup and release — overall Medium

#### T6.1 Buildroot dependency and repository purge — Medium

- Use dependency graphs plus `rg` to remove the old Python assistant packages,
  dead HA/wake package sources, obsolete docs/assets, and unselected fork debris.
- Trim ALSA plugins/tools and unused CLI utilities only from measured need.
  Decide whether Bluetooth/AP provisioning stays based on the observed Wi-Fi
  onboarding path, not on whether Cortana uses it.
- Rename the application, Buildroot package, init script, PID/config paths, and
  Docker image from HA/upstream names to Cortana/TRSPK names in one mechanical
  ticket after functionality is stable.

#### T6.2 Production access and update hardening — Medium

- Disable ADB over TCP, remove the default SSH password, remove unauthenticated
  local services, enforce credential permissions, and verify log redaction.
- Retain USB recovery and SWUpdate, use signed artifacts and SHA-256, and test
  watchdog restart/backoff without a reboot loop.

#### T6.3 Failure matrix, soak, and release — Medium

- Exercise Wi-Fi/DNS loss, clock-not-ready, Cortana restart, expired/replayed
  tickets, wrong/revoked credentials, Piper/Whisper failure, queue pressure,
  Pulse failure, and device reboot.
- Run at least a 24-hour connected voice soak and record final image size, boot
  time, CPU/memory, source/target sample rates, frames captured/sent/dropped,
  average KiB/s, peak capture/session/playback buffers, latency, and AEC quality.
- Produce signed recoverable `.swu` and `.img` artifacts plus exact USB recovery
  and provisioning documentation.

Gate: the verification contract below passes on the production image.

### Phase 7: optional full-duplex interruption — overall High

#### T7.1 AEC and barge-in experiment — High

- Confirm AEC convergence while response audio plays at representative volumes,
  rooms, distances, and speaker/microphone orientations.
- Change `bargeInMode` to `full_duplex` and verify server-side "Cortana"
  interruption without persistent false wakes from speaker echo.
- Revert to `none` if acoustics are unreliable. Extra-high is warranted only if
  measured failures point below the application into Pulse/ALSA/driver timing;
  ordinary voice endpoint delivery must not wait for this optional ticket.

## Verification contract

The work is complete only when all of the following are observed on the real
device:

- cold boot reaches authenticated, armed state without Home Assistant;
- the server assigns the configured satellite to the correct trusted area;
- microphone PCM is continuously framed at 16 kHz mono PCM16/20 ms without an
  unbounded queue;
- "Cortana" is detected by the server at representative room distances;
- speech, tools, and response audio remain isolated to the correct room;
- response PCM plays fully and playback completion is acknowledged after drain;
- mute stops transmission and cancels the active turn;
- the dedicated mute-button LED accurately reflects physical mute state without
  using or overriding the ring;
- the home button manually activates or cancels according to state;
- LED states distinguish ready, listening, thinking, speaking, reconnecting,
  blocked, and update states without competing ring writers;
- Wi-Fi and Cortana restarts recover without rebooting the device or replaying
  stale audio;
- a failed or stalled capture worker recovers with bounded backoff, without a
  reboot or stale audio;
- revoked or mismatched credentials fail closed and show a blocked status;
- no Sendspin process, package, configuration, persistence, or LED writer
  remains in the production image;
- no PCM, credential, ticket, transcript, or response text appears in device
  logs or persistent files;
- production firmware has no unauthenticated ADB TCP or default SSH password;
- a known-good full image and USB recovery procedure exist.

## Relevant upstream paths

Inspect these paths before restructuring:

```text
buildroot/package/thirdreality/linux-voice-assistant-cpp/
buildroot/package/thirdreality/linux-voice-assistant-cpp/src/main.cpp
buildroot/package/thirdreality/linux-voice-assistant-cpp/src/audio/
buildroot/package/thirdreality/linux-voice-assistant-cpp/src/satellite/
buildroot/package/thirdreality/linux-voice-assistant-cpp/src/tr/
buildroot/package/thirdreality/tr-ledring/
buildroot/configs/3reality_trspk_defconfig
buildroot/board/thirdreality/trspk/
go
Dockerfile
.github/workflows/build.yml
```

In Kensho, the server sources of truth are currently:

```text
cortana/src/api.py
cortana/src/settings.py
cortana/src/voice/capabilities.py
cortana/src/voice/contract.py
cortana/src/voice/gateway.py
cortana/src/voice/audio.py
cortana/docs/voice-protocol-v1.md
cortana/docs/operations.md
kensho-manifests/apps/cortana/core/configmap.yaml
kensho-manifests/apps/cortana/core/deployment.yaml
kensho-manifests/apps/external-secrets-config/cortana-secrets.yaml
```

This document embeds the protocol needed to build the endpoint, so access to
Kensho is not required for the early firmware phases. Before live activation,
compare the implementation with the current Kensho protocol source in case the
server contract changed.
