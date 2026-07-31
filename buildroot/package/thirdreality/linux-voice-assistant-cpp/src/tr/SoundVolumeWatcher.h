#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <sys/stat.h>

namespace lva::tr {

// Watches the sound configuration written by the TRSPK's input_eventd
// button handler and applies speaker-volume changes to the running endpoint.
class SoundVolumeWatcher {
public:
    using Clock = std::chrono::steady_clock;
    using ApplyFn = std::function<void(int percent, bool feedback)>;

    SoundVolumeWatcher(std::filesystem::path path, ApplyFn apply);

    // Loads the persisted volume without producing user feedback. This keeps
    // startup quiet while still restoring the last selected level.
    void ApplyInitial();

    void Poll(Clock::time_point now = Clock::now());

private:
    struct FileStamp {
        ino_t inode = 0;
        off_t size = 0;
        timespec modified{};

        bool operator==(const FileStamp& other) const {
            return inode == other.inode && size == other.size &&
                modified.tv_sec == other.modified.tv_sec &&
                modified.tv_nsec == other.modified.tv_nsec;
        }
    };

    std::optional<FileStamp> ReadStamp() const;
    void ReloadAndApply(bool feedback);

    std::filesystem::path path_;
    ApplyFn apply_;
    std::optional<FileStamp> last_stamp_;
    std::optional<int> applied_percent_;
    Clock::time_point last_check_{};
};

}  // namespace lva::tr
