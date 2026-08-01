#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$root_dir/buildroot/package/thirdreality/linux-voice-assistant-cpp/src"
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

${CXX:-g++} -std=c++20 -Wall -Wextra -Wpedantic -pthread \
    -I "$source_dir" \
    "$source_dir/tests/CaptureQueueTest.cpp" \
    "$source_dir/audio/CaptureFrame.cpp" \
    "$source_dir/audio/MicrophoneIngress.cpp" \
    "$source_dir/audio/PlaybackCaptureGate.cpp" \
    "$source_dir/audio/PcmRingBuffer.cpp" \
    -o "$work_dir/capture-pipeline-test"

"$work_dir/capture-pipeline-test"

plugins_mk="$root_dir/buildroot/package/alsa-plugins/alsa-plugins.mk"
defconfig="$root_dir/buildroot/configs/3reality_trspk_defconfig"
capture_header="$source_dir/audio/AudioCapture.h"
capture_source="$source_dir/audio/PulseAudioSource.cpp"
main_cpp="$source_dir/main.cpp"
cmake_file="$root_dir/buildroot/package/thirdreality/linux-voice-assistant-cpp/CMakeLists.txt"
asound_conf="$root_dir/buildroot/board/thirdreality/trspk/rootfs/etc/asound.conf"
grep -q 'std::string pulse_source = "alsa_input.hw_0_2"' "$capture_header"
grep -q 'pa_stream_connect_record' "$capture_source"
grep -q 'pa_threaded_mainloop_wait' "$capture_source"
grep -q 'src/audio/PulseAudioSource.cpp' "$cmake_file"
grep -q 'PkgConfig::PULSE' "$cmake_file"
grep -q '"capture-source"' "$main_cpp"
if grep -q 'snd_pcm_readi' "$source_dir/audio/AudioCapture.cpp"; then
    echo "production capture still uses the blocking ALSA PulseAudio bridge" >&2
    exit 1
fi
if grep -q '^pcm.cortana_capture {' "$asound_conf"; then
    echo "obsolete ALSA PulseAudio capture bridge is still installed" >&2
    exit 1
fi
if grep -q 'capture-alsa-device' "$main_cpp"; then
    echo "obsolete ALSA capture option is still exposed" >&2
    exit 1
fi
grep -q 'BR2_PACKAGE_ALSA_PLUGINS=y' "$defconfig"
grep -q 'BR2_PACKAGE_PULSEAUDIO=y' "$defconfig"
grep -q -- '--enable-pulseaudio' "$plugins_mk"
grep -Eq '^ALSA_PLUGINS_DEPENDENCIES \+= pulseaudio$' \
    "$plugins_mk"
if grep -q -- '--enable-pulseaduio' "$plugins_mk"; then
    echo "alsa-plugins still contains the misspelled PulseAudio option" >&2
    exit 1
fi
