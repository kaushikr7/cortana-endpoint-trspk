#include "tr/SoundVolumeWatcher.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "state/Preferences.h"
#include "util/Log.h"

namespace lva::tr {

namespace {

constexpr const char* kTag = "sound_volume";
constexpr auto kPollInterval = std::chrono::milliseconds(100);

}  // namespace

SoundVolumeWatcher::SoundVolumeWatcher(
    std::filesystem::path path, ApplyFn apply)
    : path_(std::move(path)), apply_(std::move(apply)) {}

void SoundVolumeWatcher::ApplyInitial() {
    last_stamp_ = ReadStamp();
    ReloadAndApply(false);
}

void SoundVolumeWatcher::Poll(Clock::time_point now) {
    if (last_check_.time_since_epoch().count() != 0 &&
        now - last_check_ < kPollInterval) {
        return;
    }
    last_check_ = now;

    const auto stamp = ReadStamp();
    if (!stamp.has_value() || stamp == last_stamp_) return;
    last_stamp_ = stamp;
    ReloadAndApply(true);
}

std::optional<SoundVolumeWatcher::FileStamp>
SoundVolumeWatcher::ReadStamp() const {
    struct stat info {};
    if (::stat(path_.c_str(), &info) != 0) return std::nullopt;
    return FileStamp{
        .inode = info.st_ino,
        .size = info.st_size,
        .modified = info.st_mtim,
    };
}

void SoundVolumeWatcher::ReloadAndApply(bool feedback) {
    const auto preferences =
        lva::state::Preferences::LoadFromFile(path_);
    if (!preferences.volume.has_value()) return;

    const int percent = std::clamp(
        static_cast<int>(std::lround(*preferences.volume * 100.0)), 0, 100);
    if (applied_percent_ == percent) return;

    LVA_LOGI(kTag, "%s volume %d%%",
             feedback ? "button changed" : "restoring", percent);
    applied_percent_ = percent;
    if (apply_) apply_(percent, feedback);
}

}  // namespace lva::tr
