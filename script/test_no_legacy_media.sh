#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cpp_root="$root_dir/buildroot/package/thirdreality/linux-voice-assistant-cpp"
defconfig="$root_dir/buildroot/configs/3reality_trspk_defconfig"

fail() {
    echo "legacy-media purge check failed: $*" >&2
    exit 1
}

for removed_path in \
    "$cpp_root/src/audio/IAudioPlayer.h" \
    "$cpp_root/src/audio/LibMpvPlayer.h" \
    "$cpp_root/src/audio/LibMpvPlayer.cpp" \
    "$cpp_root/sounds"; do
    [ ! -e "$removed_path" ] || fail "removed path remains: $removed_path"
done

if rg -n 'BR2_PACKAGE_(MPV|FFMPEG)' "$defconfig" "$cpp_root/Config.in" ||
   rg -n 'LibMpvPlayer|PkgConfig::MPV|pkg_check_modules\(MPV|(^|[[:space:]])mpv([[:space:]]|$)|thirdreality/sounds' \
       "$cpp_root/CMakeLists.txt" "$cpp_root/linux-voice-assistant-cpp.mk" \
       "$cpp_root/src"; then
    fail "MPV, FFmpeg, or legacy sound references remain"
fi

rg -q 'sample_rate > 48000' "$cpp_root/src/cortana/Protocol.cpp" ||
    fail "protocol output sample-rate ceiling is not 48 kHz"
rg -q 'sample_rate <= 48000' "$cpp_root/src/audio/RawPcmPlayer.cpp" ||
    fail "player output sample-rate ceiling is not 48 kHz"

echo "legacy-media purge checks passed"
