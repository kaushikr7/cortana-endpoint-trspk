#include "audio/PlaybackCaptureGate.h"

#include <stdexcept>

namespace lva::audio {

PlaybackCaptureGate::PlaybackCaptureGate(
    std::chrono::milliseconds echo_tail)
    : echo_tail_(echo_tail) {
    if (echo_tail_ < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("negative playback echo tail");
    }
}

bool PlaybackCaptureGate::Update(bool playback_active,
                                 Clock::time_point now) noexcept {
    if (playback_active) {
        was_active_ = true;
        return true;
    }
    if (was_active_) {
        was_active_ = false;
        resume_at_ = now + echo_tail_;
    }
    return now < resume_at_;
}

}  // namespace lva::audio
