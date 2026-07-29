#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$root_dir/buildroot/package/thirdreality/linux-voice-assistant-cpp/src"
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

${CXX:-g++} -std=c++20 -Wall -Wextra -Wpedantic -pthread \
    -I "$source_dir" \
    "$source_dir/tests/DeviceControlsTest.cpp" \
    "$source_dir/tr/HomeButton.cpp" \
    "$source_dir/tr/LedController.cpp" \
    "$source_dir/tr/MicMuteGpio.cpp" \
    "$source_dir/util/Log.cpp" \
    -o "$work_dir/device-controls-test"

"$work_dir/device-controls-test"
