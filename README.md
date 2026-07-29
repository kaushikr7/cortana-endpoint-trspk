# ThirdReality Voice Assistant

<div align="center">
  <img src="doc/images/voice-music-speaker.jpg" alt="voice-music-speaker" width="300">
</div>

ThirdReality Voice Assistant is an open-source speaker firmware derived from
the ThirdReality Voice&Music Assistant. This fork is being converted into a
dedicated Cortana endpoint; see [the build plan](doc/build.md) for the current
architecture and implementation sequence.


## Architecture

The current firmware voice application is:

- **Voice** — built on [linux-voice-assistant-cpp](buildroot/package/thirdreality/linux-voice-assistant-cpp/), a C++ rewrite of [OHF-Voice/linux-voice-assistant](https://github.com/OHF-Voice/linux-voice-assistant.git). Implements the ESPHome native API so Home Assistant discovers the speaker as a voice satellite. See its [README](buildroot/package/thirdreality/linux-voice-assistant-cpp/README.md) for details.

<div align="center">
  <img src="doc/images/button_functions.png" alt="button-functions" width="600">
</div>

---

- [ThirdReality Voice Assistant](#thirdreality-voice-assistant)
  - [Architecture](#architecture)
  - [Build](#build)
    - [Docker Build](#docker-build)
    - [Native Build](#native-build)
  - [Flash](#flash)
  - [Debugging](#debugging)
    - [Serial](#serial)
    - [ADB](#adb)
    - [SSH](#ssh)
  - [Provision a Cortana endpoint](#provision-a-cortana-endpoint)
  - [Setup the voice assist](#setup-the-voice-assist)
  - [Setup through HA APP](#setup-through-ha-app)
  - [Smart Home control with voice](#smart-home-control-with-voice)
  - [Smart Home control with button](#smart-home-control-with-button)

---

## Build

Clone the repository:
```bash
git clone https://github.com/thirdreality/voice-music-assistant.git
cd <YOUR PATH>/voice-music-assistant
git submodule update --init
```

### Docker Build

No host dependencies required other than Docker.

```bash
./go --docker trspk <version>          # Build inside Docker (recommended)
./go --docker-shell                    # Enter container interactively for debugging
./go --docker trspk rebuild <package>  # Rebuild a single package in Docker
```

Run the host-only physical-control tests without building firmware:

```bash
./script/test_device_controls.sh
./script/test_cortana_protocol.sh
./script/test_device_ticket.sh
./script/test_cortana_session.sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s tools/tests -p 'test_*.py'
```

The T2.1 fake session has no runtime dependency for its unit tests. To expose it
as a local WebSocket server for later client work, install
`websockets>=13,<16` in a disposable host environment and run:

```bash
./tools/fake_cortana_server.py
```

### Native Build

Requires:
  - Ubuntu 20.04

Install dependencies:
```bash
sudo apt-get update

sudo apt-get install -y build-essential bash bc binutils build-essential bzip2 cpio g++ gcc git gzip locales libncurses5-dev libdevmapper-dev libsystemd-dev make mercurial whois patch perl python rsync sed tar vim unzip wget bison flex libssl-dev libc6:i386 libncurses5:i386 libstdc++6:i386 zlib1g-dev:i386 zip python3-pip pkg-config automake gsettings-ubuntu-schemas libglib2.0-dev gcc-multilib g++-multilib

pip install pycrypto

wget http://ftp.cn.debian.org/debian/pool/main/a/automake-1.16/automake_1.16.1-4_all.deb && sudo dpkg -i automake_1.16.1-4_all.deb && rm -f automake_1.16.1-4_all.deb
```

Build:
```bash
./go trspk <version>               # If no version number is specified, the date will be used
./go trspk rebuild <package>       # Rebuild a single package
```

The generated image is located at:
```
<YOUR PATH>/voice-music-assistant/image
```

## Flash
1. Download and extract [Aml_Burn_Tool.zip](https://raw.githubusercontent.com/thirdreality/voice-music-assistant/master/tools/Aml_Burn_Tool.zip)

2. If this is your first time using the tool, click on Setup_Aml_Burn_Tool_V3.1.0.exe to install necessary drivers.

3. Next, navigate to the v2 folder and run Aml_Burn_Tool.exe.

4. Load the compiled **.img firmware file. Or you can download the latest firmware [here](https://github.com/thirdreality/voice-music-assistant/releases).

5. Click on Start to initiate the burn process.

<div align="left">
  <img src="doc/images/usb_burnning_tool.png" alt="usb_burnning_tool" width="400">
</div>

6. Use debug board to connect the speaker to the PC. If you don’t have a debug board, you can use a Type-C data cable. Make sure to use a data cable. Then power it on.

<div align="left">
  <img src="doc/images/device_connect.jpg" alt="device-connect" width="400">
</div>


## Debugging

### Serial

1. Use debug board to connect the speaker to the PC. Make sure to use data cables.

2. Open your serial debugging tool, select the corresponding port, and set the baud rate to 115200.

<div align="left">
  <img src="doc/images/serial-debug.png" alt="serial-debug" width="400">
</div>

### ADB

ADB is enabled on development images and starts automatically at boot
(`/etc/init.d/S55adbd`). It listens over **USB** and, by default, also over
**TCP** on port `5555`.

> **Warning:** the TCP socket binds to `0.0.0.0` with no authentication.
> Anyone on the same network can obtain a root shell. To disable TCP, set `ADB_TCP_PORT=` in `/etc/default/adbd`
> (USB stays available).

**USB** — connect a Type-C data cable to the PC:

```bash
adb devices
adb shell
```

**TCP** — over the network (device and PC on the same LAN):

```bash
adb connect <device-ip>:5555
adb shell
```

**Multiple devices** — when more than one device is connected, `adb shell`
fails with `more than one device/emulator`. You must first list the devices and
then target one explicitly with `-s <serial>`. The serial differs by transport:

- **USB** devices are listed by their **MAC address** (for example `a1b2c3d4e5f6`).
- **TCP** devices are listed as **`<ip>:5555`** (for example `10.1.0.33:5555`).

```bash
$ adb devices
List of devices attached
a1b2c3d4e5f6    device        # USB, serial is the MAC address
10.1.0.33:5555  device        # TCP, serial is <ip>:5555

# pick the USB device by its MAC address
adb -s a1b2c3d4e5f6 shell

# pick the TCP device by its <ip>:5555
adb -s 10.1.0.33:5555 shell
```

Note: the same speaker connected over both USB and TCP shows up as two separate
entries with different serials.

### SSH

```bash
ssh root@<device-ip>
```

- Username: `root`
- Password: `hello3r`

## Provision a Cortana endpoint

Provision through USB ADB after installing firmware containing the T1.2
configuration commands. The credential is requested with a no-echo prompt; it
is deliberately not accepted as a command-line argument:

Firmware burning and provisioning can use the same USB cable, but not at the
same time. Burn the image in Amlogic USB mode, boot the device normally, wait
for USB ADB, and only then provision it. Do not assume `/data/cortana` survives
a full image burn, erase/repartition operation, or factory reset; run `status`
after every flash and reprovision when it reports unconfigured.

```bash
./script/provision_cortana_endpoint.py provision \
  --serial <usb-serial> \
  --endpoint https://cortana.raintreeresearch.com \
  --satellite-id study-voice-1 \
  --expected-area-id study
```

The tool atomically installs `/data/cortana/config.json` and
`/data/cortana/credential` with mode `0600`, then asks the endpoint binary to
validate them. Inspect redacted status or rotate only the credential with:

```bash
./script/provision_cortana_endpoint.py status --serial <usb-serial>
./script/provision_cortana_endpoint.py rotate-credential --serial <usb-serial>
```

## Setup the voice assist

There are two ways to use the voice assistant: Home Assistant Cloud or local voice recognition (If your device doesn't have sufficient performance, please choose Home Assistant Cloud)

After completing either of the above options, add an assistant under **Settings → Voice Assistants**.

- Home Assistant Cloud

  Open the Home Assistant app or the Home Assistant web interface, Go to **Settings → Home Assistant Cloud**. Create or log in to your account. (30 day free trial)
  <div align="left">
    <img src="doc/images/ha-cloud-1.png" width="10%">
    <img src="doc/images/ha-cloud-2.png" width="10%">
  </div>


- Local voice recognition

  Please refer to:

  <https://github.com/rhasspy/wyoming-piper>

  <https://github.com/rhasspy/wyoming-faster-whisper>

---

## Setup through HA APP

1. You need to install the iOS or Android version of the [Home Assistant app](https://companion.home-assistant.io/) first. And please make sure the app is up to date.
2. Make sure the speaker is in a yellow blinking state. Otherwise, please try factory reset. (Press and hold the Home button for 15 seconds, then release it after you hear the prompt sound)
3. Open the Home Assistant app on your phone. Go to **Settings → Devices & services** and under Discovered, you should see the device as **"3RSPK-XXXXX Improv via BLE"**. (If the device is not found, please check whether Bluetooth and Nearby Devices permissions are enabled in the app)

<div align="left">
  <img src="doc/images/setup-1.png" width="10%">
</div>

4. Enter your Wi-Fi SSID and password. Only 2.4 GHz networks are supported.

<div align="left">
  <img src="doc/images/setup-2.png" width="10%">
  <img src="doc/images/setup-3.png" width="10%">
</div>

5. A few seconds after the Wi-Fi connection is successful, the speaker will play "Your device is ready to connect to Home Assistant." Go to **Settings → Devices & services** and under Discovered, you should see the device as **"3RSPK-XXXXXXXXXXXX ESPHome"**.

<div align="left">
  <img src="doc/images/setup-4.png" width="10%">
</div>

6. Add device

<div align="left">
  <img src="doc/images/setup-5.png" width="10%">
  <img src="doc/images/setup-6.png" width="10%">
  <img src="doc/images/setup-7.png" width="10%">
  <img src="doc/images/setup-8.png" width="10%">
  <img src="doc/images/setup-9.png" width="10%">
</div>

7. Select the voice assistant you created in step 1 (Setup the voice assist)

<div align="left">
  <img src="doc/images/setup-10.png" width="10%">
</div>

Now you can try waking the device with **"OK Nabu"** and start a conversation. You can check the device status in **Settings → Devices & Services → ESPHome**.

<div align="left">
  <img src="doc/images/setup-11.png" width="10%">
  <img src="doc/images/setup-12.png" width="10%">
</div>

---

## Smart Home control with voice

Supported voice commands: <https://www.home-assistant.io/voice_control/builtin_sentences/>

- For example, *"What's the time"* or *"Turn on the light in the living room"*.
- Make sure you're using the area name exactly as you defined it in Home Assistant.

Is the device you want to control via Assist (for example a specific light) not responding to your voice commands? Make sure the device is exposed to Assist:
<https://www.home-assistant.io/voice_control/voice_remote_expose_devices/>

---

## Smart Home control with button

We can create automation scripts based on the speaker's Home button trigger events to control devices. Supports single-click, double-click, and triple-click actions.

**Settings → Devices & Services → ESPHome → Your device → Automations**

<div align="left">
  <img src="doc/images/button-control-1.png" width="30%">
  <img src="doc/images/button-control-2.png" width="30%">
  <img src="doc/images/button-control-3.png" width="30%">
  <img src="doc/images/button-control-4.png" width="30%">
  <img src="doc/images/button-control-5.png" width="30%">
</div>
