#include "cortana/EndpointState.h"

#include <type_traits>

namespace lva::cortana {

void EndpointState::ResetTurn(Activity activity) {
    snapshot_.activity = activity;
    snapshot_.active_turn_id.reset();
    snapshot_.playback_turn_id.reset();
}

void EndpointState::UpdateSession(const SessionSnapshot& session) {
    const bool connection_changed =
        snapshot_.generation != session.generation ||
        snapshot_.phase != session.phase;
    const bool health_changed =
        observed_session_health_ != session.health ||
        observed_session_activity_ != session.activity;

    snapshot_.phase = session.phase;
    snapshot_.generation = session.generation;
    snapshot_.health = session.health;
    observed_session_health_ = session.health;
    observed_session_activity_ = session.activity;

    if (session.phase != SessionPhase::Ready) {
        ResetTurn();
        return;
    }
    if (connection_changed || health_changed) {
        snapshot_.activity = session.activity;
    }
}

void EndpointState::HandleServerEvent(const SessionEvent& event) {
    if (event.generation != snapshot_.generation ||
        snapshot_.phase != SessionPhase::Ready) {
        return;
    }

    std::visit(
        [this](const auto& value) {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, SessionReady>) {
                snapshot_.health = value.health;
                snapshot_.activity = value.activity;
            } else if constexpr (std::is_same_v<Event, SessionHealth>) {
                snapshot_.health = value.health;
                snapshot_.activity = value.activity;
                observed_session_health_ = value.health;
                observed_session_activity_ = value.activity;
            } else if constexpr (std::is_same_v<Event, WakeAccepted>) {
                snapshot_.active_turn_id = value.turn_id;
                snapshot_.activity = Activity::Hearing;
            } else if constexpr (std::is_same_v<Event, WakeSuppressed>) {
                ResetTurn(Activity::Armed);
            } else if constexpr (std::is_same_v<Event, SpeechStarted>) {
                snapshot_.active_turn_id = value.turn_id;
                snapshot_.activity = Activity::Hearing;
            } else if constexpr (std::is_same_v<Event, SpeechEnded>) {
                snapshot_.active_turn_id = value.turn_id;
                snapshot_.activity = Activity::Transcribing;
            } else if constexpr (std::is_same_v<Event, TranscriptFinal> ||
                                 std::is_same_v<Event, ResponseStarted> ||
                                 std::is_same_v<Event, ResponseText>) {
                snapshot_.active_turn_id = value.turn_id;
                snapshot_.activity = Activity::Thinking;
            } else if constexpr (std::is_same_v<Event,
                                                ResponseCompleted>) {
                snapshot_.active_turn_id = value.turn_id;
                if (!snapshot_.playback_turn_id.has_value()) {
                    ResetTurn(Activity::Armed);
                }
            } else if constexpr (std::is_same_v<Event,
                                                OutputAudioStart>) {
                snapshot_.active_turn_id = value.turn_id;
                snapshot_.playback_turn_id = value.turn_id;
                snapshot_.activity = Activity::Thinking;
            } else if constexpr (std::is_same_v<Event, PlaybackStop>) {
                snapshot_.active_turn_id = value.turn_id;
                snapshot_.playback_turn_id = value.turn_id;
                snapshot_.activity = Activity::Interrupting;
            } else if constexpr (std::is_same_v<Event, TurnCancelled>) {
                if (!value.turn_id.has_value() ||
                    snapshot_.active_turn_id == value.turn_id) {
                    ResetTurn(Activity::Armed);
                }
            }
        },
        event.event);
}

void EndpointState::HandlePlaybackResult(
    const lva::audio::RawPlaybackResult& result) {
    if (snapshot_.phase != SessionPhase::Ready) return;
    if (!snapshot_.playback_turn_id.has_value() ||
        *snapshot_.playback_turn_id != result.turn_id) {
        return;
    }
    if (result.outcome == lva::audio::RawPlaybackOutcome::Started) {
        snapshot_.active_turn_id = result.turn_id;
        snapshot_.playback_turn_id = result.turn_id;
        snapshot_.activity = Activity::Speaking;
        return;
    }
    ResetTurn(Activity::Armed);
}

void EndpointState::BeginCancellation(
    const std::optional<std::string>& turn_id) {
    if (snapshot_.phase != SessionPhase::Ready) return;
    if (turn_id.has_value() && snapshot_.active_turn_id.has_value() &&
        *turn_id != *snapshot_.active_turn_id) {
        return;
    }
    snapshot_.activity = Activity::Interrupting;
}

void EndpointState::SetMuted(bool muted) {
    snapshot_.muted = muted;
    if (muted) ResetTurn(Activity::Armed);
}

std::optional<std::string> EndpointState::ActiveTurnId() const {
    if (snapshot_.active_turn_id.has_value()) {
        return snapshot_.active_turn_id;
    }
    return snapshot_.playback_turn_id;
}

}  // namespace lva::cortana
