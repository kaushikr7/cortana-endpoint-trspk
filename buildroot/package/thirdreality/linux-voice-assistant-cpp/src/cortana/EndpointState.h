#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "audio/RawPcmPlayer.h"
#include "cortana/SessionClient.h"

namespace lva::cortana {

struct EndpointSnapshot {
    SessionPhase phase = SessionPhase::Stopped;
    Health health = Health::Starting;
    Activity activity = Activity::Idle;
    std::uint64_t generation = 0;
    bool muted = false;
    std::optional<std::string> active_turn_id;
    std::optional<std::string> playback_turn_id;
};

class EndpointState {
public:
    void UpdateSession(const SessionSnapshot& session);
    void HandleServerEvent(const SessionEvent& event);
    void HandlePlaybackResult(const lva::audio::RawPlaybackResult& result);
    void BeginCancellation(const std::optional<std::string>& turn_id);
    void SetMuted(bool muted);

    EndpointSnapshot Snapshot() const { return snapshot_; }
    std::optional<std::string> ActiveTurnId() const;

private:
    void ResetTurn(Activity activity = Activity::Idle);

    EndpointSnapshot snapshot_;
    Health observed_session_health_ = Health::Starting;
    Activity observed_session_activity_ = Activity::Idle;
};

}  // namespace lva::cortana
