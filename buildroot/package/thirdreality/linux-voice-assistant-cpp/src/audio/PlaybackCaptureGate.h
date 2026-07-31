#pragma once

#include <chrono>

namespace lva::audio {

// Prevents speaker output and its short room tail from being returned to the
// server on endpoints that deliberately do not support acoustic barge-in.
class PlaybackCaptureGate {
public:
    using Clock = std::chrono::steady_clock;

    explicit PlaybackCaptureGate(
        std::chrono::milliseconds echo_tail = std::chrono::milliseconds{300});

    bool Update(bool playback_active,
                Clock::time_point now = Clock::now()) noexcept;

private:
    std::chrono::milliseconds echo_tail_;
    Clock::time_point resume_at_{};
    bool was_active_ = false;
};

}  // namespace lva::audio
