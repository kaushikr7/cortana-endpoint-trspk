#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>

#include "audio/AudioCapture.h"

namespace lva::audio {

enum class CaptureLifecycleState { Stopped, Starting, Ready, Degraded, Blocked };
enum class CaptureFailure { None, StartFailed, Exited, Stalled };

struct CaptureSupervisorSnapshot {
    CaptureLifecycleState state = CaptureLifecycleState::Stopped;
    CaptureFailure last_failure = CaptureFailure::None;
    std::uint64_t start_attempts = 0;
    std::uint64_t recovery_boundaries = 0;
    std::uint64_t planned_restarts = 0;
    std::uint64_t exited_workers = 0;
    std::uint64_t stalled_workers = 0;
    std::uint32_t consecutive_failures = 0;
    bool retry_scheduled = false;
    std::uint64_t retry_in_ms = 0;
};

class CaptureSupervisor {
public:
    using Clock = std::chrono::steady_clock;

    struct Dependencies {
        std::function<bool()> start;
        std::function<void()> stop;
        std::function<void()> flush_audio;
        std::function<AudioCaptureMetrics()> metrics;
    };

    struct Options {
        std::chrono::milliseconds startup_timeout{2000};
        std::chrono::milliseconds stall_timeout{2000};
        std::chrono::milliseconds stable_reset_after{30000};
        std::chrono::milliseconds initial_backoff{1000};
        std::chrono::milliseconds maximum_backoff{30000};
        std::uint32_t blocked_after_failures = 3;
    };

    explicit CaptureSupervisor(Dependencies dependencies);
    CaptureSupervisor(Dependencies dependencies, Options options);

    void Start(Clock::time_point now = Clock::now());
    void Poll(Clock::time_point now = Clock::now());
    void RestartAfterHardwareChange(Clock::time_point now = Clock::now());
    void Stop();

    CaptureSupervisorSnapshot Snapshot(
        Clock::time_point now = Clock::now()) const;

private:
    static std::uint64_t MonotonicNanoseconds(Clock::time_point now) noexcept;
    void AttemptStart(Clock::time_point now);
    void RegisterFailure(CaptureFailure failure, Clock::time_point now);
    std::chrono::milliseconds RetryDelay() const noexcept;
    void SetState(CaptureLifecycleState state);

    Dependencies dependencies_;
    Options options_;
    CaptureSupervisorSnapshot snapshot_;
    Clock::time_point attempt_started_{};
    Clock::time_point ready_since_{};
    Clock::time_point retry_at_{};
    bool active_ = false;
};

std::string_view ToString(CaptureLifecycleState state);
std::string_view ToString(CaptureFailure failure);

}  // namespace lva::audio
