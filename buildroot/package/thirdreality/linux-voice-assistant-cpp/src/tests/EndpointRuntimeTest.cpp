#include <cassert>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortana/EndpointRuntime.h"

namespace {

using Json = nlohmann::json;
using lva::cortana::EndpointRuntime;
using lva::cortana::SessionEvent;
using lva::cortana::SessionPhase;
using lva::cortana::SessionSnapshot;

struct FakePorts {
    bool accept_commands = true;
    std::vector<Json> sent;
    std::vector<bool> mute_states;
    int capture_discards = 0;
    std::vector<std::string> playback_begins;
    std::vector<std::string> playback_chunks;
    std::vector<std::string> playback_ends;
    std::vector<std::pair<std::string, std::string>> playback_stops;
    int activations = 0;

    EndpointRuntime::Dependencies Dependencies() {
        return {
            .send_command = [this](std::string payload) {
                if (!accept_commands) return false;
                sent.push_back(Json::parse(payload));
                return true;
            },
            .set_microphone_muted = [this](bool muted) {
                mute_states.push_back(muted);
            },
            .discard_capture = [this] { ++capture_discards; },
            .begin_playback = [this](
                std::string turn_id, lva::audio::PcmFormat) {
                playback_begins.push_back(std::move(turn_id));
                return true;
            },
            .enqueue_playback = [this](
                std::string_view turn_id, std::string payload) {
                playback_chunks.push_back(
                    std::string(turn_id) + ":" + payload);
                return true;
            },
            .end_playback = [this](std::string_view turn_id) {
                playback_ends.emplace_back(turn_id);
                return true;
            },
            .stop_playback = [this](std::string turn_id,
                                    std::string reason) {
                playback_stops.emplace_back(std::move(turn_id),
                                            std::move(reason));
            },
            .new_activation_id = [this] {
                return "manual-" + std::to_string(++activations);
            },
        };
    }
};

SessionSnapshot Ready(std::uint64_t generation = 1) {
    SessionSnapshot result;
    result.phase = SessionPhase::Ready;
    result.health = lva::cortana::Health::Ready;
    result.activity = lva::cortana::Activity::Armed;
    result.generation = generation;
    return result;
}

template<typename Event>
void Server(EndpointRuntime& runtime, Event event,
            std::uint64_t generation = 1) {
    runtime.HandleServerEvent(SessionEvent{
        .generation = generation,
        .event = std::move(event),
    });
}

lva::cortana::OutputAudioStart AudioStart(std::string turn_id) {
    return {
        .turn_id = std::move(turn_id),
        .encoding = "pcm_s16le",
        .sample_rate = 24000,
        .sample_width = 2,
        .channels = 1,
    };
}

void ClearHandshake(FakePorts& ports, EndpointRuntime& runtime) {
    runtime.UpdateSession(Ready());
    assert(ports.sent.size() == 1);
    assert(ports.sent[0].at("type") == "mute.changed");
    ports.sent.clear();
}

void TestPlaybackDispatchAndPhysicalAcknowledgements() {
    FakePorts ports;
    EndpointRuntime runtime(ports.Dependencies());
    ClearHandshake(ports, runtime);
    Server(runtime,
           lva::cortana::WakeAccepted{"activation-1", "turn-1"});
    Server(runtime, AudioStart("turn-1"));
    Server(runtime, lva::cortana::OutputAudioChunk{
        .turn_id = "turn-1",
        .payload = "\x01\x02",
    });
    Server(runtime, lva::cortana::OutputAudioEnd{"turn-1"});
    assert(ports.playback_begins == std::vector<std::string>{"turn-1"});
    assert(ports.playback_chunks.size() == 1);
    assert(ports.playback_ends == std::vector<std::string>{"turn-1"});

    runtime.HandlePlaybackResult({
        .turn_id = "turn-1",
        .outcome = lva::audio::RawPlaybackOutcome::Started,
        .detail = {},
    });
    runtime.HandlePlaybackResult({
        .turn_id = "turn-1",
        .outcome = lva::audio::RawPlaybackOutcome::Completed,
        .detail = {},
    });
    assert(ports.sent.size() == 2);
    assert(ports.sent[0].at("type") == "playback.started");
    assert(ports.sent[1].at("type") == "playback.completed");

    ports.sent.clear();
    Server(runtime,
           lva::cortana::WakeAccepted{"activation-2", "turn-2"});
    Server(runtime, AudioStart("turn-2"));
    runtime.OnHomeButton(lva::tr::HomeButtonPress::Single);
    assert(ports.sent.empty());
    assert(ports.playback_stops.back().second == "user_cancelled");
    runtime.HandlePlaybackResult({
        .turn_id = "turn-2",
        .outcome = lva::audio::RawPlaybackOutcome::Stopped,
        .detail = "user_cancelled",
    });
    assert(ports.sent.size() == 1);
    assert(ports.sent[0].at("type") == "playback.stopped");
}

void TestMuteOrderingAndNonPlaybackCancellation() {
    FakePorts ports;
    EndpointRuntime runtime(ports.Dependencies());
    ClearHandshake(ports, runtime);
    Server(runtime,
           lva::cortana::WakeAccepted{"activation-1", "turn-1"});
    Server(runtime, AudioStart("turn-1"));
    runtime.OnMuteChanged(true);
    assert(ports.sent.empty());
    assert(ports.capture_discards == 1);
    assert(ports.mute_states == std::vector<bool>{true});
    runtime.HandlePlaybackResult({
        .turn_id = "turn-1",
        .outcome = lva::audio::RawPlaybackOutcome::Stopped,
        .detail = "muted",
    });
    assert(ports.sent.size() == 2);
    assert(ports.sent[0].at("type") == "playback.stopped");
    assert(ports.sent[1].at("type") == "mute.changed");
    assert(ports.sent[1].at("muted") == true);

    FakePorts hearing_ports;
    EndpointRuntime hearing(hearing_ports.Dependencies());
    ClearHandshake(hearing_ports, hearing);
    Server(hearing,
           lva::cortana::WakeAccepted{"activation-2", "turn-2"});
    hearing.OnMuteChanged(true);
    assert(hearing_ports.sent.size() == 2);
    assert(hearing_ports.sent[0].at("type") == "turn.cancel");
    assert(hearing_ports.sent[0].at("turnId") == "turn-2");
    assert(hearing_ports.sent[0].at("source") == "mute");
    assert(hearing_ports.sent[1].at("type") == "mute.changed");
}

void TestCommandPressureAndGenerationIsolation() {
    FakePorts ports;
    ports.accept_commands = false;
    EndpointRuntime runtime(ports.Dependencies());
    runtime.UpdateSession(Ready());
    assert(runtime.Metrics().queued_commands == 1);
    ports.accept_commands = true;
    runtime.PumpCommands();
    assert(ports.sent.size() == 1);
    assert(runtime.Metrics().queued_commands == 0);

    ports.sent.clear();
    Server(runtime, AudioStart("turn-old"));
    auto disconnected = Ready(2);
    disconnected.phase = SessionPhase::Backoff;
    disconnected.health = lva::cortana::Health::Reconnecting;
    runtime.UpdateSession(disconnected);
    assert(ports.playback_stops.back().second == "session_disconnected");
    runtime.UpdateSession(Ready(2));
    ports.sent.clear();  // generation-2 mute sync
    runtime.HandlePlaybackResult({
        .turn_id = "turn-old",
        .outcome = lva::audio::RawPlaybackOutcome::Stopped,
        .detail = "old_generation",
    });
    assert(ports.sent.empty());

    runtime.OnHomeButton(lva::tr::HomeButtonPress::Single);
    assert(ports.sent.size() == 1);
    assert(ports.sent[0].at("type") == "wake.manual");
    assert(ports.sent[0].at("activationId") == "manual-1");
}

}  // namespace

int main() {
    TestPlaybackDispatchAndPhysicalAcknowledgements();
    TestMuteOrderingAndNonPlaybackCancellation();
    TestCommandPressureAndGenerationIsolation();
}
