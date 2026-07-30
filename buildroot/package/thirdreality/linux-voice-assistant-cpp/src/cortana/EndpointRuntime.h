#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "audio/PcmSink.h"
#include "audio/RawPcmPlayer.h"
#include "cortana/EndpointState.h"
#include "tr/HomeButton.h"
#include "tr/PhysicalControlPolicy.h"

namespace lva::cortana {

struct EndpointRuntimeMetrics {
    std::size_t queued_commands = 0;
    std::uint64_t commands_sent = 0;
    std::uint64_t commands_dropped = 0;
};

class EndpointRuntime {
public:
    struct Dependencies {
        std::function<bool(std::string)> send_command;
        std::function<void(bool)> set_microphone_muted;
        std::function<void()> discard_capture;
        std::function<bool(std::string, lva::audio::PcmFormat)>
            begin_playback;
        std::function<bool(std::string_view, std::string)> enqueue_playback;
        std::function<bool(std::string_view)> end_playback;
        std::function<void(std::string, std::string)> stop_playback;
        std::function<std::string()> new_activation_id;
    };

    struct Options {
        std::size_t maximum_queued_commands = 16;
    };

    explicit EndpointRuntime(Dependencies dependencies);
    EndpointRuntime(Dependencies dependencies, Options options);

    void UpdateSession(const SessionSnapshot& session);
    void HandleServerEvent(const SessionEvent& event);
    void HandlePlaybackResult(const lva::audio::RawPlaybackResult& result);
    void OnMuteChanged(bool muted);
    void OnHomeButton(lva::tr::HomeButtonPress press);
    void PumpCommands();

    EndpointSnapshot Snapshot() const { return state_.Snapshot(); }
    EndpointRuntimeMetrics Metrics() const;

private:
    struct PendingCommand {
        std::uint64_t generation;
        std::string payload;
    };
    struct PendingPlaybackStop {
        std::uint64_t generation;
        std::string turn_id;
        bool notify_muted = false;
    };

    static lva::tr::EndpointActivity EndpointActivityFor(
        const EndpointSnapshot& snapshot);
    void QueueCommand(std::string payload, std::uint64_t generation);
    void StopForDisconnectedSession();

    Dependencies dependencies_;
    Options options_;
    EndpointState state_;
    SessionSnapshot session_;
    std::uint64_t playback_generation_ = 0;
    std::string playback_turn_id_;
    std::optional<PendingPlaybackStop> pending_playback_stop_;
    std::deque<PendingCommand> commands_;
    std::uint64_t mute_synced_generation_ = 0;
    std::uint64_t commands_sent_ = 0;
    std::uint64_t commands_dropped_ = 0;
};

}  // namespace lva::cortana
