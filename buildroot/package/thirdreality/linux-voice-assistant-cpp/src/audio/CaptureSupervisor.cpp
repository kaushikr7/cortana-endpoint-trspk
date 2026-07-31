#include "audio/CaptureSupervisor.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "util/Log.h"

namespace lva::audio {

namespace {

constexpr const char* kTag = "capture_supervisor";

}  // namespace

CaptureSupervisor::CaptureSupervisor(Dependencies dependencies)
    : CaptureSupervisor(std::move(dependencies), Options{}) {}

CaptureSupervisor::CaptureSupervisor(Dependencies dependencies,
                                     Options options)
    : dependencies_(std::move(dependencies)), options_(options) {
    if (!dependencies_.start || !dependencies_.stop ||
        !dependencies_.flush_audio || !dependencies_.metrics ||
        options_.startup_timeout <= std::chrono::milliseconds::zero() ||
        options_.stall_timeout <= std::chrono::milliseconds::zero() ||
        options_.stable_reset_after <= std::chrono::milliseconds::zero() ||
        options_.initial_backoff <= std::chrono::milliseconds::zero() ||
        options_.maximum_backoff < options_.initial_backoff ||
        options_.blocked_after_failures == 0) {
        throw std::invalid_argument("invalid capture supervisor options");
    }
}

std::uint64_t CaptureSupervisor::MonotonicNanoseconds(
    Clock::time_point now) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());
}

void CaptureSupervisor::SetState(CaptureLifecycleState state) {
    if (snapshot_.state == state) return;
    snapshot_.state = state;
    LVA_LOGI(kTag, "state=%.*s failure=%.*s consecutive_failures=%u",
             static_cast<int>(ToString(state).size()), ToString(state).data(),
             static_cast<int>(ToString(snapshot_.last_failure).size()),
             ToString(snapshot_.last_failure).data(),
             snapshot_.consecutive_failures);
}

void CaptureSupervisor::Start(Clock::time_point now) {
    if (active_) return;
    active_ = true;
    dependencies_.flush_audio();
    AttemptStart(now);
}

void CaptureSupervisor::AttemptStart(Clock::time_point now) {
    ++snapshot_.start_attempts;
    snapshot_.retry_scheduled = false;
    snapshot_.retry_in_ms = 0;
    attempt_started_ = now;
    SetState(CaptureLifecycleState::Starting);
    if (!dependencies_.start()) {
        RegisterFailure(CaptureFailure::StartFailed, now);
    }
}

std::chrono::milliseconds CaptureSupervisor::RetryDelay() const noexcept {
    auto delay = options_.initial_backoff;
    for (std::uint32_t count = 1;
         count < snapshot_.consecutive_failures &&
         delay < options_.maximum_backoff;
         ++count) {
        delay = std::min(delay * 2, options_.maximum_backoff);
    }
    return delay;
}

void CaptureSupervisor::RegisterFailure(CaptureFailure failure,
                                        Clock::time_point now) {
    dependencies_.stop();
    dependencies_.flush_audio();
    ++snapshot_.recovery_boundaries;
    ++snapshot_.consecutive_failures;
    snapshot_.last_failure = failure;
    if (failure == CaptureFailure::Exited) ++snapshot_.exited_workers;
    if (failure == CaptureFailure::Stalled) ++snapshot_.stalled_workers;
    retry_at_ = now + RetryDelay();
    snapshot_.retry_scheduled = true;
    SetState(snapshot_.consecutive_failures >= options_.blocked_after_failures
                 ? CaptureLifecycleState::Blocked
                 : CaptureLifecycleState::Degraded);
}

void CaptureSupervisor::Poll(Clock::time_point now) {
    if (!active_) return;

    if (snapshot_.state == CaptureLifecycleState::Degraded ||
        snapshot_.state == CaptureLifecycleState::Blocked) {
        if (now >= retry_at_) AttemptStart(now);
        return;
    }

    const AudioCaptureMetrics metrics = dependencies_.metrics();
    if (snapshot_.state == CaptureLifecycleState::Starting) {
        if (!metrics.running) {
            RegisterFailure(CaptureFailure::Exited, now);
            return;
        }
        if (metrics.last_period_monotonic_ns != 0) {
            const std::uint64_t now_ns = MonotonicNanoseconds(now);
            if (now_ns >= metrics.last_period_monotonic_ns &&
                now_ns - metrics.last_period_monotonic_ns <=
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            options_.stall_timeout).count())) {
                ready_since_ = now;
                SetState(CaptureLifecycleState::Ready);
                return;
            }
        }
        if (now - attempt_started_ >= options_.startup_timeout) {
            RegisterFailure(CaptureFailure::Stalled, now);
        }
        return;
    }

    if (snapshot_.state != CaptureLifecycleState::Ready) return;
    if (!metrics.running) {
        RegisterFailure(CaptureFailure::Exited, now);
        return;
    }
    const std::uint64_t now_ns = MonotonicNanoseconds(now);
    const auto stall_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            options_.stall_timeout).count());
    if (metrics.last_period_monotonic_ns == 0 ||
        now_ns < metrics.last_period_monotonic_ns ||
        now_ns - metrics.last_period_monotonic_ns > stall_ns) {
        RegisterFailure(CaptureFailure::Stalled, now);
        return;
    }
    if (snapshot_.consecutive_failures != 0 &&
        now - ready_since_ >= options_.stable_reset_after) {
        snapshot_.consecutive_failures = 0;
        snapshot_.last_failure = CaptureFailure::None;
    }
}

void CaptureSupervisor::RestartAfterHardwareChange(Clock::time_point now) {
    if (!active_) return;
    dependencies_.stop();
    dependencies_.flush_audio();
    ++snapshot_.recovery_boundaries;
    ++snapshot_.planned_restarts;
    snapshot_.last_failure = CaptureFailure::None;
    snapshot_.consecutive_failures = 0;
    AttemptStart(now);
}

void CaptureSupervisor::Stop() {
    if (!active_) return;
    active_ = false;
    dependencies_.stop();
    snapshot_.retry_scheduled = false;
    snapshot_.retry_in_ms = 0;
    SetState(CaptureLifecycleState::Stopped);
}

CaptureSupervisorSnapshot CaptureSupervisor::Snapshot(
    Clock::time_point now) const {
    CaptureSupervisorSnapshot result = snapshot_;
    if (result.retry_scheduled) {
        result.retry_in_ms = now >= retry_at_
            ? 0
            : static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      retry_at_ - now).count());
    }
    return result;
}

std::string_view ToString(CaptureLifecycleState state) {
    switch (state) {
        case CaptureLifecycleState::Stopped:
            return "stopped";
        case CaptureLifecycleState::Starting:
            return "starting";
        case CaptureLifecycleState::Ready:
            return "ready";
        case CaptureLifecycleState::Degraded:
            return "degraded";
        case CaptureLifecycleState::Blocked:
            return "blocked";
    }
    return "unknown";
}

std::string_view ToString(CaptureFailure failure) {
    switch (failure) {
        case CaptureFailure::None:
            return "none";
        case CaptureFailure::StartFailed:
            return "start_failed";
        case CaptureFailure::Exited:
            return "worker_exited";
        case CaptureFailure::Stalled:
            return "worker_stalled";
    }
    return "unknown";
}

}  // namespace lva::audio
