#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$root_dir/buildroot/package/thirdreality/linux-voice-assistant-cpp/src"
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

${CXX:-g++} -std=c++20 -Wall -Wextra -Wpedantic -pthread \
    -I "$source_dir" \
    "$source_dir/tests/EndpointStateTest.cpp" \
    "$source_dir/cortana/EndpointState.cpp" \
    -o "$work_dir/endpoint-state-test"

"$work_dir/endpoint-state-test"
