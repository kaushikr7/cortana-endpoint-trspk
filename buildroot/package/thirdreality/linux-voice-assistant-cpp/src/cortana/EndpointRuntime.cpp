#include "cortana/EndpointRuntime.h"

#include <stdexcept>
#include <type_traits>
#include <utility>

#include "cortana/Protocol.h"
#include "tr/PhysicalControlPolicy.h"
#include "util/Log.h"

namespace lva::cortana {

namespace {

constexpr const char* kTag = "runtime";

}  // namespace

EndpointRuntime::EndpointRuntime(Dependencies dependencies)
    : EndpointRuntime(std::move(dependencies), Options{}) {}

EndpointRuntime::EndpointRuntime(Dependencies dependencies, Options options)
    : dependencies_(std::move(dependencies)), options_(std::move(options)) {
    if (!dependencies_.send_command ||
        !dependencies_.set_microphone_muted ||
        !dependencies_.discard_capture ||
        !dependencies_.begin_playback ||
        !dependencies_.enqueue_playback ||
        !dependencies_.end_playback ||
        !dependencies_.stop_playback ||
        !dependencies_.new_activation_id ||
        options_.maximum_queued_commands == 0) {
        throw std::invalid_argument("invalid endpoint runtime dependencies");
    }
}

lva::tr::EndpointActivity EndpointRuntime::EndpointActivityFor(
    const EndpointSnapshot& snapshot) {
    if (snapshot.phase != SessionPhase::Ready) {
        return lva::tr::EndpointActivity::Unavailable;
    }
    if (snapshot.playback_turn_id.has_value() ||
        snapshot.activity == Activity::Speaking ||
        snapshot.activity == Activity::Interrupting) {
        return lva::tr::EndpointActivity::Playback;
    }
    if (snapshot.active_turn_id.has_value()) {
        return lva::tr::EndpointActivity::ActiveTurn;
    }
    if (snapshot.activity == Activity::Armed ||
        snapshot.activity == Activity::Idle) {
        return lva::tr::EndpointActivity::Armed;
    }
    return lva::tr::EndpointActivity::ActiveTurn;
}

void EndpointRuntime::QueueCommand(std::string payload,
                                   std::uint64_t generation) {
    if (payload.empty() || generation == 0 ||
        commands_.size() >= options_.maximum_queued_commands) {
        ++commands_dropped_;
        LVA_LOGW(kTag, "command queue rejected payload generation=%llu",
                 static_cast<unsigned long long>(generation));
        return;
    }
    commands_.push_back({generation, std::move(payload)});
}

void EndpointRuntime::StopForDisconnectedSession() {
    if (playback_generation_ != 0) {
        dependencies_.stop_playback({}, "session_disconnected");
    }
    playback_generation_ = 0;
    playback_turn_id_.clear();
    pending_playback_stop_.reset();
    commands_dropped_ += commands_.size();
    commands_.clear();
}

void EndpointRuntime::UpdateSession(const SessionSnapshot& session) {
    session_ = session;
    state_.UpdateSession(session_);
    if (playback_generation_ != 0 &&
        (session_.phase != SessionPhase::Ready ||
         session_.generation != playback_generation_)) {
        StopForDisconnectedSession();
    } else if (session_.phase != SessionPhase::Ready) {
        commands_dropped_ += commands_.size();
        commands_.clear();
        pending_playback_stop_.reset();
    }
    if (session_.phase == SessionPhase::Ready &&
        mute_synced_generation_ != session_.generation &&
        (!pending_playback_stop_.has_value() ||
         !pending_playback_stop_->notify_muted)) {
        QueueCommand(SerializeMuteChanged(state_.Snapshot().muted),
                     session_.generation);
        mute_synced_generation_ = session_.generation;
    }
    PumpCommands();
}

void EndpointRuntime::HandleServerEvent(const SessionEvent& event) {
    if (event.generation != session_.generation ||
        session_.phase != SessionPhase::Ready) {
        return;
    }
    state_.HandleServerEvent(event);
    std::visit(
        [this](const auto& value) {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, OutputAudioStart>) {
                const bool accepted = dependencies_.begin_playback(
                    value.turn_id,
                    lva::audio::PcmFormat{
                        .encoding = value.encoding,
                        .sample_rate = value.sample_rate,
                        .sample_width = value.sample_width,
                        .channels = value.channels,
                    });
                playback_generation_ = session_.generation;
                playback_turn_id_ = value.turn_id;
                if (!accepted) {
                    LVA_LOGE(kTag, "rejected audio.start turn=%s",
                             value.turn_id.c_str());
                    dependencies_.stop_playback(
                        value.turn_id, "invalid_audio_start");
                }
            } else if constexpr (std::is_same_v<Event, OutputAudioChunk>) {
                if (!dependencies_.enqueue_playback(
                        value.turn_id, value.payload)) {
                    LVA_LOGE(kTag,
                             "playback queue rejected %zu bytes turn=%s",
                             value.payload.size(), value.turn_id.c_str());
                    dependencies_.stop_playback(
                        value.turn_id, "playback_overloaded");
                }
            } else if constexpr (std::is_same_v<Event, OutputAudioEnd>) {
                if (!dependencies_.end_playback(value.turn_id)) {
                    LVA_LOGW(kTag, "ignored audio.end turn=%s",
                             value.turn_id.c_str());
                }
            } else if constexpr (std::is_same_v<Event, PlaybackStop>) {
                dependencies_.stop_playback(value.turn_id, value.reason);
            } else if constexpr (std::is_same_v<Event, TurnCancelled>) {
                dependencies_.stop_playback(
                    value.turn_id.value_or(std::string{}),
                    value.reason.value_or("turn_cancelled"));
                if (!value.turn_id.has_value() ||
                    *value.turn_id == playback_turn_id_) {
                    playback_generation_ = 0;
                    playback_turn_id_.clear();
                    pending_playback_stop_.reset();
                }
            }
        },
        event.event);
    PumpCommands();
}

void EndpointRuntime::HandlePlaybackResult(
    const lva::audio::RawPlaybackResult& result) {
    state_.HandlePlaybackResult(result);
    const bool current = session_.phase == SessionPhase::Ready &&
        session_.generation == playback_generation_ &&
        result.turn_id == playback_turn_id_;
    if (current) {
        if (result.outcome == lva::audio::RawPlaybackOutcome::Started) {
            QueueCommand(SerializePlaybackStarted(result.turn_id),
                         playback_generation_);
        } else if (result.outcome ==
                   lva::audio::RawPlaybackOutcome::Completed) {
            QueueCommand(SerializePlaybackCompleted(result.turn_id),
                         playback_generation_);
        } else {
            QueueCommand(SerializePlaybackStopped(
                             result.turn_id,
                             result.outcome ==
                                     lva::audio::RawPlaybackOutcome::Error
                                 ? "playback_error"
                                 : "playback_stopped"),
                         playback_generation_);
        }
    }

    if (result.outcome != lva::audio::RawPlaybackOutcome::Started &&
        pending_playback_stop_.has_value() &&
        pending_playback_stop_->generation == session_.generation &&
        pending_playback_stop_->turn_id == result.turn_id) {
        if (pending_playback_stop_->notify_muted) {
            QueueCommand(SerializeMuteChanged(true),
                         pending_playback_stop_->generation);
            mute_synced_generation_ = pending_playback_stop_->generation;
        }
        pending_playback_stop_.reset();
    }
    if (result.outcome != lva::audio::RawPlaybackOutcome::Started &&
        result.turn_id == playback_turn_id_) {
        playback_generation_ = 0;
        playback_turn_id_.clear();
    }
    PumpCommands();
}

void EndpointRuntime::OnMuteChanged(bool muted) {
    const EndpointSnapshot before = state_.Snapshot();
    const bool state_changed = before.muted != muted;
    const auto decision = lva::tr::PhysicalControlPolicy::OnMuteChanged(
        muted, state_changed, EndpointActivityFor(before));
    const auto active_turn = state_.ActiveTurnId();
    const bool can_notify_server =
        before.phase == SessionPhase::Ready && before.generation != 0;
    dependencies_.set_microphone_muted(muted);
    state_.SetMuted(muted);

    if (muted) {
        dependencies_.discard_capture();
        if (decision.cancel_turn && before.playback_turn_id.has_value()) {
            pending_playback_stop_ = PendingPlaybackStop{
                .generation = before.generation,
                .turn_id = *before.playback_turn_id,
                .notify_muted = true,
            };
        }
        dependencies_.stop_playback({}, "muted");
    }
    if (can_notify_server && decision.cancel_turn &&
        !before.playback_turn_id.has_value()) {
        QueueCommand(SerializeTurnCancel(
                         active_turn, CancellationSource::Mute,
                         "microphone_muted"),
                     before.generation);
    }
    if (can_notify_server && decision.notify_server &&
        !(muted && pending_playback_stop_.has_value())) {
        QueueCommand(SerializeMuteChanged(muted), before.generation);
    }
    PumpCommands();
}

void EndpointRuntime::OnHomeButton(lva::tr::HomeButtonPress press) {
    const EndpointSnapshot snapshot = state_.Snapshot();
    const auto action = lva::tr::PhysicalControlPolicy::OnHomeButton(
        press, EndpointActivityFor(snapshot));
    if (action == lva::tr::ControlAction::ManualWake) {
        QueueCommand(SerializeWakeManual(dependencies_.new_activation_id()),
                     snapshot.generation);
    } else if (action == lva::tr::ControlAction::CancelTurn) {
        state_.BeginCancellation(state_.ActiveTurnId());
        dependencies_.stop_playback({}, "user_cancelled");
        if (snapshot.playback_turn_id.has_value()) {
            pending_playback_stop_ = PendingPlaybackStop{
                .generation = snapshot.generation,
                .turn_id = *snapshot.playback_turn_id,
                .notify_muted = false,
            };
        } else {
            QueueCommand(SerializeTurnCancel(
                             state_.ActiveTurnId(),
                             CancellationSource::Physical,
                             "user_cancelled"),
                         snapshot.generation);
        }
    }
    PumpCommands();
}

void EndpointRuntime::PumpCommands() {
    while (!commands_.empty()) {
        if (session_.phase != SessionPhase::Ready ||
            commands_.front().generation != session_.generation) {
            commands_.pop_front();
            ++commands_dropped_;
            continue;
        }
        if (!dependencies_.send_command(commands_.front().payload)) break;
        commands_.pop_front();
        ++commands_sent_;
    }
}

EndpointRuntimeMetrics EndpointRuntime::Metrics() const {
    return {
        .queued_commands = commands_.size(),
        .commands_sent = commands_sent_,
        .commands_dropped = commands_dropped_,
    };
}

}  // namespace lva::cortana
