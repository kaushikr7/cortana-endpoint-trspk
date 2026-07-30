#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$root_dir/buildroot/package/thirdreality/linux-voice-assistant-cpp/src"
corpus="$root_dir/testdata/voice_protocol_v1_conformance.json"
expected_sha256=5a49d8e9cc7580626f7d121ab4c2fa505dc0ee4ce725ab4994ad1b72db5e1e26
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

actual_sha256=$(sha256sum "$corpus" | awk '{print $1}')
if [ "$actual_sha256" != "$expected_sha256" ]; then
    echo "voice protocol corpus checksum mismatch" >&2
    echo "expected: $expected_sha256" >&2
    echo "actual:   $actual_sha256" >&2
    exit 1
fi

if [ -n "${NLOHMANN_INCLUDE_DIR:-}" ]; then
    json_include=$NLOHMANN_INCLUDE_DIR
elif [ -f /usr/include/nlohmann/json.hpp ]; then
    json_include=/usr/include
else
    json_include="$work_dir/include"
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

${CXX:-g++} -std=c++20 -Wall -Wextra -Wpedantic \
    -I "$source_dir" \
    -I "$json_include" \
    "$source_dir/tests/ProtocolConformanceTest.cpp" \
    "$source_dir/cortana/Protocol.cpp" \
    -o "$work_dir/protocol-conformance-test"

"$work_dir/protocol-conformance-test" "$corpus"
