#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>

#include "audio/CaptureSupervisor.h"

namespace {

using namespace std::chrono_literals;
using lva::audio::AudioCaptureMetrics;
using lva::audio::CaptureFailure;
using lva::audio::CaptureLifecycleState;
using lva::audio::CaptureSupervisor;

std::uint64_t Nanoseconds(CaptureSupervisor::Clock::time_point now) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());
}

struct Fixture {
    AudioCaptureMetrics metrics;
    std::deque<bool> start_results;
    int starts = 0;
    int stops = 0;
    int flushes = 0;

    CaptureSupervisor MakeSupervisor() {
        return CaptureSupervisor(
            {
                .start = [this] {
                    ++starts;
                    const bool result = start_results.empty()
                        ? true : start_results.front();
                    if (!start_results.empty()) start_results.pop_front();
                    metrics.running = result;
                    metrics.last_period_monotonic_ns = 0;
                    return result;
                },
                .stop = [this] {
                    ++stops;
                    metrics.running = false;
                },
                .flush_audio = [this] { ++flushes; },
                .metrics = [this] { return metrics; },
            },
            {
                .startup_timeout = 100ms,
                .stall_timeout = 100ms,
                .stable_reset_after = 1000ms,
                .initial_backoff = 10ms,
                .maximum_backoff = 40ms,
                .blocked_after_failures = 3,
            });
    }
};

void MarkPeriod(Fixture& fixture, CaptureSupervisor::Clock::time_point now) {
    fixture.metrics.running = true;
    fixture.metrics.last_period_monotonic_ns = Nanoseconds(now);
}

void TestStartReadyExitAndFreshRecovery() {
    Fixture fixture;
    auto supervisor = fixture.MakeSupervisor();
    const auto start = CaptureSupervisor::Clock::time_point{} + 1s;

    supervisor.Start(start);
    assert(fixture.starts == 1);
    assert(fixture.flushes == 1);
    assert(supervisor.Snapshot(start).state == CaptureLifecycleState::Starting);

    MarkPeriod(fixture, start + 5ms);
    supervisor.Poll(start + 5ms);
    assert(supervisor.Snapshot(start + 5ms).state ==
           CaptureLifecycleState::Ready);

    fixture.metrics.running = false;
    supervisor.Poll(start + 6ms);
    auto snapshot = supervisor.Snapshot(start + 6ms);
    assert(snapshot.state == CaptureLifecycleState::Degraded);
    assert(snapshot.last_failure == CaptureFailure::Exited);
    assert(snapshot.exited_workers == 1);
    assert(snapshot.recovery_boundaries == 1);
    assert(fixture.stops == 1);
    assert(fixture.flushes == 2);

    supervisor.Poll(start + 15ms);
    assert(fixture.starts == 1);
    supervisor.Poll(start + 16ms);
    assert(fixture.starts == 2);
    assert(supervisor.Snapshot(start + 16ms).state ==
           CaptureLifecycleState::Starting);
    MarkPeriod(fixture, start + 17ms);
    supervisor.Poll(start + 17ms);
    assert(supervisor.Snapshot(start + 17ms).state ==
           CaptureLifecycleState::Ready);

    MarkPeriod(fixture, start + 1018ms);
    supervisor.Poll(start + 1018ms);
    snapshot = supervisor.Snapshot(start + 1018ms);
    assert(snapshot.consecutive_failures == 0);
    assert(snapshot.last_failure == CaptureFailure::None);
}

void TestStallsBackOffAndEventuallyBlock() {
    Fixture fixture;
    auto supervisor = fixture.MakeSupervisor();
    auto now = CaptureSupervisor::Clock::time_point{} + 2s;
    supervisor.Start(now);

    for (int failure = 1; failure <= 3; ++failure) {
        MarkPeriod(fixture, now + 1ms);
        supervisor.Poll(now + 1ms);
        supervisor.Poll(now + 102ms);
        const auto failed = supervisor.Snapshot(now + 102ms);
        assert(failed.consecutive_failures ==
               static_cast<std::uint32_t>(failure));
        assert(failed.last_failure == CaptureFailure::Stalled);
        assert(failed.state == (failure == 3
            ? CaptureLifecycleState::Blocked
            : CaptureLifecycleState::Degraded));
        const auto delay = failure == 1 ? 10ms : failure == 2 ? 20ms : 40ms;
        supervisor.Poll(now + 102ms + delay - 1ms);
        assert(fixture.starts == failure);
        supervisor.Poll(now + 102ms + delay);
        assert(fixture.starts == failure + 1);
        now = now + 102ms + delay;
    }

    assert(supervisor.Snapshot(now).stalled_workers == 3);
    assert(fixture.flushes == 4);
}

void TestFailedStartIsRetriedWithoutTightLoop() {
    Fixture fixture;
    fixture.start_results = {false, true};
    auto supervisor = fixture.MakeSupervisor();
    const auto start = CaptureSupervisor::Clock::time_point{} + 3s;
    supervisor.Start(start);

    auto snapshot = supervisor.Snapshot(start);
    assert(snapshot.state == CaptureLifecycleState::Degraded);
    assert(snapshot.last_failure == CaptureFailure::StartFailed);
    assert(snapshot.retry_scheduled);
    supervisor.Poll(start + 9ms);
    assert(fixture.starts == 1);
    supervisor.Poll(start + 10ms);
    assert(fixture.starts == 2);
}

void TestHardwareChangeRestartsWithFreshAudioImmediately() {
    Fixture fixture;
    auto supervisor = fixture.MakeSupervisor();
    const auto start = CaptureSupervisor::Clock::time_point{} + 4s;
    supervisor.Start(start);
    MarkPeriod(fixture, start + 1ms);
    supervisor.Poll(start + 1ms);

    supervisor.RestartAfterHardwareChange(start + 2ms);
    const auto snapshot = supervisor.Snapshot(start + 2ms);
    assert(snapshot.state == CaptureLifecycleState::Starting);
    assert(snapshot.planned_restarts == 1);
    assert(snapshot.recovery_boundaries == 1);
    assert(snapshot.consecutive_failures == 0);
    assert(snapshot.last_failure == CaptureFailure::None);
    assert(fixture.starts == 2);
    assert(fixture.stops == 1);
    assert(fixture.flushes == 2);
}

}  // namespace

int main() {
    TestStartReadyExitAndFreshRecovery();
    TestStallsBackOffAndEventuallyBlock();
    TestFailedStartIsRetriedWithoutTightLoop();
    TestHardwareChangeRestartsWithFreshAudioImmediately();
}
