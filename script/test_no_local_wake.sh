#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
package_root="$root_dir/buildroot/package/thirdreality"
cpp_root="$package_root/linux-voice-assistant-cpp"

fail() {
    echo "local-wake purge check failed: $*" >&2
    exit 1
}

for removed_path in \
    "$package_root/linux-voice-assistant" \
    "$package_root/pyopen-wakeword" \
    "$package_root/pyring-buffer" \
    "$package_root/python-pymicro-features" \
    "$package_root/python-pymicro-wakeword" \
    "$cpp_root/third_party/microfrontend" \
    "$cpp_root/third_party/tflite_c" \
    "$cpp_root/wakewords"; do
    [ ! -e "$removed_path" ] || fail "removed path remains: $removed_path"
done

if find "$package_root" -type f \
        \( -iname '*.tflite' -o -iname 'libtensorflowlite*' \) \
        -print -quit | grep -q .; then
    fail "a TFLite model or runtime library remains"
fi

if rg -n \
        'MicroWakeWord|OpenWakeWord|ExternalWakeWord|MicroFeatures|TfliteRuntime|WakeWordEngine|WakeWordScanner|microfrontend|tensorflowlite|BR2_PACKAGE_(PYOPEN_WAKEWORD|PYTHON_PYMICRO|PYRING_BUFFER)' \
        "$root_dir/buildroot/configs" \
        "$package_root/Config.in" \
        "$cpp_root/CMakeLists.txt" \
        "$cpp_root/linux-voice-assistant-cpp.mk" \
        "$cpp_root/src" >/dev/null; then
    fail "a local wake implementation or Buildroot reference remains"
fi

[ -f "$cpp_root/src/tools/aec_loopback_test.cpp" ] || \
    fail "AEC loopback diagnostic source was removed"
rg -q '^add_executable\(aec_loopback_test$' "$cpp_root/CMakeLists.txt" || \
    fail "AEC loopback diagnostic target is not built"
rg -q '^install\(TARGETS aec_loopback_test ' "$cpp_root/CMakeLists.txt" || \
    fail "AEC loopback diagnostic target is not installed"

echo "local-wake purge checks passed"
