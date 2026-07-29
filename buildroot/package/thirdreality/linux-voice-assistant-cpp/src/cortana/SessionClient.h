#pragma once

#include <chrono>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "config/EndpointConfig.h"
#include "cortana/Protocol.h"
#include "cortana/SessionTransport.h"

namespace lva::cortana {

enum class SessionPhase {
    Stopped,
    Connecting,
    Negotiating,
    Ready,
    Backoff,
    Blocked,
};

struct SessionSnapshot {
    SessionPhase phase = SessionPhase::Stopped;
    Health health = Health::Starting;
    Activity activity = Activity::Idle;
    std::string session_id;
    std::string detail;
    std::uint64_t generation = 0;
    std::size_t reconnect_attempt = 0;
    std::size_t queued_commands = 0;
    std::uint64_t dropped_commands = 0;
    bool audio_started = false;
    bool microphone_muted = false;
    std::size_t queued_audio_frames = 0;
    std::uint64_t audio_frames_sent = 0;
    std::uint64_t dropped_audio_frames = 0;
};

struct SessionEvent {
    std::uint64_t generation;
    ServerEvent event;
};

class SessionClient {
public:
    struct Options {
        std::size_t maximum_queued_commands = 32;
        std::size_t maximum_queued_events = 32;
        std::size_t maximum_queued_audio_frames = 8;
        std::size_t maximum_command_bytes = 64 * 1024;
        std::chrono::milliseconds handshake_timeout{8000};
        std::chrono::milliseconds receive_poll{10};
        std::chrono::milliseconds ping_interval{15000};
        std::chrono::milliseconds ping_timeout{10000};
        std::chrono::milliseconds stable_connection_time{60000};
        std::chrono::milliseconds initial_backoff{500};
        std::chrono::milliseconds maximum_backoff{30000};
    };

    using SteadyClock = std::chrono::steady_clock;
    using ClockFn = std::function<SteadyClock::time_point()>;
    using JitterFn = std::function<double()>;

    SessionClient(lva::config::EndpointConfig config,
                  std::shared_ptr<SessionDependencies> dependencies);
    SessionClient(lva::config::EndpointConfig config,
                  std::shared_ptr<SessionDependencies> dependencies,
                  Options options,
                  ClockFn clock,
                  JitterFn jitter);
    ~SessionClient();

    SessionClient(const SessionClient&) = delete;
    SessionClient& operator=(const SessionClient&) = delete;

    void Start();
    void Stop();

    // This is the only network-facing entry point for audio/control owners.
    // It never performs I/O and rejects stale, oversized, or excess work.
    bool EnqueueText(std::string payload);
    bool EnqueueAudioFrame(
        std::uint64_t generation,
        const std::array<std::byte, kMicrophoneFrameBytes>& frame);
    void SetMicrophoneMuted(bool muted);
    void DiscardAudioFrames();
    std::optional<SessionEvent> TryPopEvent();

    SessionSnapshot Snapshot() const;
    bool WaitForPhase(SessionPhase phase,
                      std::chrono::milliseconds timeout) const;

    static std::chrono::milliseconds ReconnectDelay(
        std::size_t attempt,
        const Options& options,
        double jitter_unit);

private:
    void Run();
    void RunConnection(const DeviceTicket& ticket,
                       SessionTransport& transport);
    void SetPhase(SessionPhase phase, std::string detail = {});
    void SetReady(const SessionReady& ready,
                  const DeviceTicket& ticket);
    void PushEvent(ServerEvent event);
    void DropQueuedCommands();
    void DropQueuedAudioLocked();
    bool WaitForStop(std::chrono::milliseconds duration);
    bool StopRequested() const;

    lva::config::EndpointConfig config_;
    std::shared_ptr<SessionDependencies> dependencies_;
    Options options_;
    ClockFn clock_;
    JitterFn jitter_;

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    SessionSnapshot snapshot_;
    std::deque<std::string> commands_;
    std::deque<std::array<std::byte, kMicrophoneFrameBytes>> audio_frames_;
    std::deque<SessionEvent> events_;
    bool stop_requested_ = false;
    std::thread worker_;
};

std::string_view ToString(SessionPhase phase);

}  // namespace lva::cortana
