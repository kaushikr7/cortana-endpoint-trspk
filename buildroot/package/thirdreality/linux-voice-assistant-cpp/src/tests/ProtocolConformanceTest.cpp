#include <cassert>
#include <fstream>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

#include "cortana/Protocol.h"

namespace {

using Json = nlohmann::json;
using lva::cortana::ProtocolError;

Json LoadCorpus(const char* path) {
    std::ifstream input(path);
    assert(input.good());
    return Json::parse(input);
}

const Json& Event(const Json& events, std::string_view type) {
    for (const auto& event : events) {
        if (event.value("type", "") == type) return event;
    }
    assert(false);
    return events.front();
}

template <typename Function>
void ExpectProtocolError(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const ProtocolError&) {
        rejected = true;
    }
    assert(rejected);
}

void CheckClientEvents(const Json& corpus) {
    using namespace lva::cortana;
    const auto& events = corpus.at("clientEvents").at("valid");
    const auto& ticket = corpus.at("ticket").at("ticket").get_ref<const std::string&>();

    assert(Json::parse(SerializeSessionAuthenticate(ticket)) ==
           Event(events, "session.authenticate"));
    assert(Json::parse(SerializeSessionCapabilities(
               ContinuousDeviceCapabilities())) ==
           Event(events, "session.capabilities"));
    assert(Json::parse(SerializeAudioStart()) == Event(events, "audio.start"));
    assert(Json::parse(SerializeWakeManual("manual-fixture")) ==
           Event(events, "wake.manual"));
    assert(Json::parse(SerializeTurnCancel(
               std::string("turn-fixture"), CancellationSource::Physical,
               "user_cancelled")) == Event(events, "turn.cancel"));
    assert(Json::parse(SerializePlaybackStarted("turn-fixture")) ==
           Event(events, "playback.started"));
    assert(Json::parse(SerializePlaybackCompleted("turn-fixture")) ==
           Event(events, "playback.completed"));
    assert(Json::parse(SerializePlaybackStopped(
               "turn-fixture", "playback_stopped")) ==
           Event(events, "playback.stopped"));
    assert(Json::parse(SerializeMuteChanged(true)) ==
           Event(events, "mute.changed"));
    assert(Json::parse(SerializeSessionPing(std::string("fixture-ping"))) ==
           Event(events, "session.ping"));
}

void CheckServerEvents(const Json& corpus) {
    using namespace lva::cortana;
    const Json& ready = corpus.at("sessionReady").at("device");
    assert(std::holds_alternative<SessionReady>(ParseServerEvent(ready.dump())));

    for (const auto& event : corpus.at("serverEvents").at("valid")) {
        (void)ParseServerEvent(event.dump());
    }
    for (const auto& event : corpus.at("serverEvents").at("invalid")) {
        ExpectProtocolError([&event] { (void)ParseServerEvent(event.dump()); });
    }
}

void CheckFramingAndOrdering(const Json& corpus) {
    using namespace lva::cortana;
    assert(corpus.at("corpusVersion") == 1);
    assert(corpus.at("protocolVersion") == kProtocolVersion);

    const auto& microphone = corpus.at("microphone");
    const auto calculated_bytes =
        microphone.at("sampleRate").get<std::size_t>() *
        microphone.at("channels").get<std::size_t>() * 2U *
        microphone.at("frameDurationMs").get<std::size_t>() / 1000U;
    assert(calculated_bytes == kMicrophoneFrameBytes);
    assert(microphone.at("frameBytes") == kMicrophoneFrameBytes);

    const auto capabilities = ParseCapabilitiesJson(
        corpus.at("capabilities").at("device").dump());
    assert(capabilities == ContinuousDeviceCapabilities());

    const auto& ordering = corpus.at("ordering");
    assert(ordering.at("responsePlayback") == Json({
        "audio.start", "binary", "audio.end", "playback.started",
        "playback.completed"}));
    assert(ordering.at("mutePlaybackCancel") ==
           Json({"playback.stopped", "mute.changed"}));
    assert(ordering.at("reconnectRule") ==
           "never acknowledge a result from an older generation");
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const Json corpus = LoadCorpus(argv[1]);
    CheckFramingAndOrdering(corpus);
    CheckClientEvents(corpus);
    CheckServerEvents(corpus);
}
