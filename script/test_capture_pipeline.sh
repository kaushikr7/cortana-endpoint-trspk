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
    "$source_dir/audio/PcmRingBuffer.cpp" \
    -o "$work_dir/capture-pipeline-test"

"$work_dir/capture-pipeline-test"
