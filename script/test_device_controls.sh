#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$root_dir/buildroot/package/thirdreality/linux-voice-assistant-cpp/src"
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

json_include=/tmp/cortana-endpoint-test-include
if [ -f "$json_include/nlohmann/json.hpp" ]; then
    :
elif [ -f /usr/include/nlohmann/json.hpp ]; then
    json_include=/usr/include
else
    mkdir -p "$json_include/nlohmann"
    json_url=https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$json_url" -o "$json_include/nlohmann/json.hpp"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$json_url" -O "$json_include/nlohmann/json.hpp"
    else
        echo "curl or wget is required to fetch the pinned nlohmann test header" >&2
        exit 1
    fi
fi

${CXX:-g++} -std=c++20 -Wall -Wextra -Wpedantic -pthread \
    -I "$source_dir" \
    -I "$json_include" \
    "$source_dir/tests/DeviceControlsTest.cpp" \
    "$source_dir/audio/ConfirmationTone.cpp" \
    "$source_dir/state/Preferences.cpp" \
    "$source_dir/tr/HomeButton.cpp" \
    "$source_dir/tr/EndpointLedPolicy.cpp" \
    "$source_dir/tr/LedController.cpp" \
    "$source_dir/tr/MicMuteGpio.cpp" \
    "$source_dir/tr/SoundVolumeWatcher.cpp" \
    "$source_dir/util/Log.cpp" \
    -o "$work_dir/device-controls-test"

"$work_dir/device-controls-test"
