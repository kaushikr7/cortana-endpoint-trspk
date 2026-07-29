#include <cassert>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

#include "cortana/Protocol.h"

namespace {

using Json = nlohmann::json;
using lva::cortana::ProtocolError;

Json CapabilitiesJson() {
    return {
        {"endpointKind", "device"},
        {"captureMode", "continuous"},
        {"wakeMode", "server"},
        {"microphone",
         {
             {"encoding", "pcm_s16le"},
             {"sampleRate", 16000},
             {"channels", 1},
             {"frameDurationMs", 20},
         }},
        {"playback", true},
        {"localPreRollMs", 0},
        {"followUpCapture", true},
        {"playbackAcknowledgements", true},
        {"bargeInMode", "none"},
    };
}

Json SessionReadyJson() {
    return {
        {"type", "session.ready"},
        {"sessionId", "session-1"},
        {"protocolVersion", "1"},
        {"satellite",
         {
             {"satelliteId", "study-voice-1"},
             {"areaId", "study"},
             {"label", "Study voice endpoint"},
         }},
        {"capabilities", CapabilitiesJson()},
        {"microphone", CapabilitiesJson().at("microphone")},
        {"health", "ready"},
        {"activity", "armed"},
    };
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

void TestClientSerialization() {
    using namespace lva::cortana;
    const std::string ticket(32, 't');
    const Json authentication = Json::parse(SerializeSessionAuthenticate(ticket));
    assert(authentication == Json({
        {"type", "session.authenticate"},
        {"protocolVersion", "1"},
        {"ticket", ticket},
    }));

    const Json capabilities = Json::parse(
        SerializeSessionCapabilities(ContinuousDeviceCapabilities()));
    assert(capabilities.at("capabilities") == CapabilitiesJson());

    const Json audio = Json::parse(SerializeAudioStart());
    assert(audio.at("frameDurationMs") == 20);
    assert(!audio.contains("activationId"));

    const Json wake = Json::parse(SerializeWakeManual("manual-1"));
    assert(wake.at("activationId") == "manual-1");
    const Json cancel = Json::parse(SerializeTurnCancel(std::nullopt));
    assert(cancel.at("source") == "physical");
    assert(cancel.at("reason") == "user_cancelled");

    ExpectProtocolError([] {
        (void)SerializeWakeManual("identifier with spaces");
    });
}

void TestStrictServerParsing() {
    using namespace lva::cortana;
    const auto ready = ParseServerEvent(SessionReadyJson().dump());
    assert(std::holds_alternative<SessionReady>(ready));
    const auto& parsed = std::get<SessionReady>(ready);
    assert(parsed.satellite.area_id == "study");
    assert(parsed.capabilities == ContinuousDeviceCapabilities());

    Json health = {
        {"type", "session.health"},
        {"health", "ready"},
        {"activity", "armed"},
        {"nonce", "ping-1"},
    };
    const auto health_event = ParseServerEvent(health.dump());
    assert(std::get<SessionHealth>(health_event).nonce == "ping-1");

    Json audio = {
        {"type", "audio.start"},
        {"turnId", "turn-1"},
        {"encoding", "pcm_s16le"},
        {"sampleRate", 22050},
        {"sampleWidth", 2},
        {"channels", 1},
    };
    assert(std::holds_alternative<OutputAudioStart>(
        ParseServerEvent(audio.dump())));

    Json extra = SessionReadyJson();
    extra["unexpected"] = true;
    ExpectProtocolError([&extra] { (void)ParseServerEvent(extra.dump()); });

    Json mismatch = SessionReadyJson();
    mismatch["capabilities"]["wakeMode"] = "satellite";
    ExpectProtocolError(
        [&mismatch] { (void)ParseServerEvent(mismatch.dump()); });
    ExpectProtocolError(
        [] { (void)ParseServerEvent(R"({"type":"new.event"})"); });
    ExpectProtocolError([] { (void)ParseServerEvent("not-json"); });
}

}  // namespace

int main() {
    TestClientSerialization();
    TestStrictServerParsing();
}
