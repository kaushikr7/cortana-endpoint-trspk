#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$root_dir/buildroot/package/thirdreality/linux-voice-assistant-cpp/src"
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

${CXX:-g++} -std=c++20 -Wall -Wextra -Wpedantic \
    -I "$source_dir" \
    "$source_dir/tests/AudioGainConfigTest.cpp" \
    -o "$work_dir/audio-gain-config-test"

"$work_dir/audio-gain-config-test"

grep -q 'cfg.gain_controller2.enabled = true' \
    "$source_dir/audio/WebRtcProcessor.cpp"
grep -q 'cfg.gain_controller2.adaptive_digital.enabled = false' \
    "$source_dir/audio/WebRtcProcessor.cpp"
if grep -q 'cfg.gain_controller1.enabled = true' \
    "$source_dir/audio/WebRtcProcessor.cpp"; then
    echo "production capture still enables the 31 dB-limited AGC1" >&2
    exit 1
fi
grep -q 'cfg.gain_controller2.enabled = true' \
    "$source_dir/tools/aec_loopback_test.cpp"
grep -q '"capture-gain-db"' "$source_dir/main.cpp"

echo "TRSPK production gain configuration checks passed"
