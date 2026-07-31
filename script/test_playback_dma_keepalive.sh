#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$root_dir/buildroot/package/thirdreality/linux-voice-assistant-cpp/src"
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

${CXX:-g++} -std=c++20 -Wall -Wextra -Wpedantic -pthread \
    -I "$source_dir" \
    "$source_dir/tests/PlaybackDmaKeepaliveTest.cpp" \
    "$source_dir/audio/PlaybackDmaKeepalive.cpp" \
    "$source_dir/util/Log.cpp" \
    -o "$work_dir/playback-dma-keepalive-test"

"$work_dir/playback-dma-keepalive-test"
