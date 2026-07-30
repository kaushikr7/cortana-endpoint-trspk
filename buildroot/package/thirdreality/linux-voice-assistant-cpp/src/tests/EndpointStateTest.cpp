#include <cassert>
#include <string>

#include "cortana/EndpointState.h"

namespace {

using lva::cortana::Activity;
using lva::cortana::EndpointState;
using lva::cortana::Health;
using lva::cortana::SessionEvent;
using lva::cortana::SessionPhase;
using lva::cortana::SessionSnapshot;

SessionSnapshot ReadySession(std::uint64_t generation = 1) {
    SessionSnapshot session;
    session.phase = SessionPhase::Ready;
    session.health = Health::Ready;
    session.activity = Activity::Armed;
    session.generation = generation;
    return session;
}

template<typename Event>
void ServerEvent(EndpointState& state, Event event,
                 std::uint64_t generation = 1) {
    state.HandleServerEvent(SessionEvent{
        .generation = generation,
        .event = std::move(event),
    });
}

void TestTurnAndPhysicalPlaybackTransitions() {
    EndpointState state;
    state.UpdateSession(ReadySession());
    assert(state.Snapshot().activity == Activity::Armed);

    ServerEvent(state, lva::cortana::WakeAccepted{"activation-1", "turn-1"});
    assert(state.Snapshot().activity == Activity::Hearing);
    assert(state.ActiveTurnId() == "turn-1");

    ServerEvent(state, lva::cortana::SpeechEnded{"turn-1"});
    assert(state.Snapshot().activity == Activity::Transcribing);
    ServerEvent(state, lva::cortana::ResponseStarted{"turn-1"});
    assert(state.Snapshot().activity == Activity::Thinking);
    ServerEvent(state, lva::cortana::OutputAudioStart{
        .turn_id = "turn-1",
        .encoding = "pcm_s16le",
        .sample_rate = 24000,
        .sample_width = 2,
        .channels = 1,
    });
    assert(state.Snapshot().activity == Activity::Thinking);

    state.HandlePlaybackResult({
        .turn_id = "turn-1",
        .outcome = lva::audio::RawPlaybackOutcome::Started,
        .detail = {},
    });
    assert(state.Snapshot().activity == Activity::Speaking);
    ServerEvent(state, lva::cortana::ResponseCompleted{"turn-1"});
    assert(state.Snapshot().activity == Activity::Speaking);
    state.HandlePlaybackResult({
        .turn_id = "turn-1",
        .outcome = lva::audio::RawPlaybackOutcome::Completed,
        .detail = {},
    });
    assert(state.Snapshot().activity == Activity::Armed);
    assert(!state.ActiveTurnId().has_value());
}

void TestCancellationMuteAndReconnectReset() {
    EndpointState state;
    state.UpdateSession(ReadySession());
    ServerEvent(state, lva::cortana::OutputAudioStart{
        .turn_id = "turn-2",
        .encoding = "pcm_s16le",
        .sample_rate = 24000,
        .sample_width = 2,
        .channels = 1,
    });
    ServerEvent(state, lva::cortana::PlaybackStop{
        .turn_id = "turn-2",
        .source = lva::cortana::CancellationSource::Physical,
        .reason = "cancelled",
    });
    assert(state.Snapshot().activity == Activity::Interrupting);
    state.HandlePlaybackResult({
        .turn_id = "turn-2",
        .outcome = lva::audio::RawPlaybackOutcome::Stopped,
        .detail = "cancelled",
    });
    assert(state.Snapshot().activity == Activity::Armed);

    ServerEvent(state, lva::cortana::WakeAccepted{"manual", "turn-local"});
    state.BeginCancellation(std::string("turn-local"));
    assert(state.Snapshot().activity == Activity::Interrupting);
    ServerEvent(state, lva::cortana::TurnCancelled{
        .turn_id = "turn-local",
        .source = lva::cortana::CancellationSource::Physical,
        .reason = "user_cancelled",
    });
    assert(state.Snapshot().activity == Activity::Armed);

    ServerEvent(state, lva::cortana::WakeAccepted{"activation-2", "turn-3"});
    state.SetMuted(true);
    assert(state.Snapshot().muted);
    assert(state.Snapshot().activity == Activity::Armed);
    assert(!state.ActiveTurnId().has_value());

    auto reconnecting = ReadySession(2);
    reconnecting.phase = SessionPhase::Backoff;
    reconnecting.health = Health::Reconnecting;
    reconnecting.activity = Activity::Idle;
    state.UpdateSession(reconnecting);
    assert(state.Snapshot().phase == SessionPhase::Backoff);
    assert(!state.ActiveTurnId().has_value());

    // Events from the retired generation cannot resurrect its turn.
    ServerEvent(state, lva::cortana::WakeAccepted{"old", "old-turn"}, 1);
    assert(!state.ActiveTurnId().has_value());

    state.UpdateSession(ReadySession(2));
    ServerEvent(state, lva::cortana::OutputAudioStart{
        .turn_id = "turn-new",
        .encoding = "pcm_s16le",
        .sample_rate = 24000,
        .sample_width = 2,
        .channels = 1,
    }, 2);
    state.HandlePlaybackResult({
        .turn_id = "old-turn",
        .outcome = lva::audio::RawPlaybackOutcome::Stopped,
        .detail = "old_generation",
    });
    assert(state.ActiveTurnId() == "turn-new");
}

}  // namespace

int main() {
    TestTurnAndPhysicalPlaybackTransitions();
    TestCancellationMuteAndReconnectReset();
}
