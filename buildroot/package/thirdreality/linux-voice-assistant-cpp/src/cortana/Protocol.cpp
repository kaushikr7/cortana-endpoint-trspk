#include "cortana/Protocol.h"

#include <algorithm>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace lva::cortana {

namespace {

using Json = nlohmann::json;

void RequireKeys(const Json& value,
                 std::initializer_list<std::string_view> required,
                 std::initializer_list<std::string_view> optional = {}) {
    if (!value.is_object()) throw ProtocolError("event must be a JSON object");
    std::set<std::string> allowed;
    for (const auto key : required) {
        allowed.emplace(key);
        if (!value.contains(key)) {
            throw ProtocolError("missing field: " + std::string(key));
        }
    }
    for (const auto key : optional) allowed.emplace(key);
    for (const auto& [key, item] : value.items()) {
        (void)item;
        if (!allowed.contains(key)) {
            throw ProtocolError("unknown field: " + key);
        }
    }
}

std::string ReadString(const Json& value,
                       std::string_view field,
                       std::size_t minimum = 0,
                       std::size_t maximum = 4096) {
    const auto it = value.find(field);
    if (it == value.end() || !it->is_string()) {
        throw ProtocolError("field must be a string: " + std::string(field));
    }
    std::string result = it->get<std::string>();
    if (result.size() < minimum || result.size() > maximum) {
        throw ProtocolError("field has invalid length: " + std::string(field));
    }
    return result;
}

bool IsIdentifier(std::string_view value) {
    return !value.empty() && value.size() <= 100 &&
        std::all_of(value.begin(), value.end(), [](char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') ||
                   character == '.' || character == '_' || character == ':' ||
                   character == '-';
        });
}

std::string ReadIdentifier(const Json& value, std::string_view field) {
    std::string result = ReadString(value, field, 1, 100);
    if (!IsIdentifier(result)) {
        throw ProtocolError("field is not a valid identifier: " +
                            std::string(field));
    }
    return result;
}

void ValidateIdentifier(std::string_view value, std::string_view field) {
    if (!IsIdentifier(value)) {
        throw ProtocolError("invalid identifier: " + std::string(field));
    }
}

void ValidateReason(std::string_view value) {
    if (value.empty() || value.size() > 80 || !IsIdentifier(value)) {
        throw ProtocolError("invalid cancellation reason");
    }
}

Json MicrophoneToJson(const MicrophoneFormat& microphone) {
    return {
        {"encoding", microphone.encoding},
        {"sampleRate", microphone.sample_rate},
        {"channels", microphone.channels},
        {"frameDurationMs", microphone.frame_duration_ms},
    };
}

MicrophoneFormat ParseMicrophone(const Json& value) {
    RequireKeys(value,
                {"encoding", "sampleRate", "channels", "frameDurationMs"});
    MicrophoneFormat result;
    result.encoding = ReadString(value, "encoding", 1, 32);
    if (!value.at("sampleRate").is_number_integer() ||
        !value.at("channels").is_number_integer() ||
        !value.at("frameDurationMs").is_number_integer()) {
        throw ProtocolError("microphone numeric fields must be integers");
    }
    result.sample_rate = value.at("sampleRate").get<int>();
    result.channels = value.at("channels").get<int>();
    result.frame_duration_ms = value.at("frameDurationMs").get<int>();
    if (result != MicrophoneFormat{}) {
        throw ProtocolError("unsupported microphone format");
    }
    return result;
}

Json CapabilitiesToJson(const Capabilities& capabilities) {
    ValidateCapabilities(capabilities);
    return {
        {"endpointKind", capabilities.endpoint_kind},
        {"captureMode", capabilities.capture_mode},
        {"wakeMode", capabilities.wake_mode},
        {"microphone", MicrophoneToJson(capabilities.microphone)},
        {"playback", capabilities.playback},
        {"localPreRollMs", capabilities.local_pre_roll_ms},
        {"followUpCapture", capabilities.follow_up_capture},
        {"playbackAcknowledgements",
         capabilities.playback_acknowledgements},
        {"bargeInMode", capabilities.barge_in_mode},
    };
}

Capabilities ParseCapabilities(const Json& value) {
    RequireKeys(value,
                {"endpointKind", "captureMode", "wakeMode", "microphone",
                 "playback", "localPreRollMs", "followUpCapture",
                 "playbackAcknowledgements", "bargeInMode"});
    Capabilities result;
    result.endpoint_kind = ReadString(value, "endpointKind", 1, 32);
    result.capture_mode = ReadString(value, "captureMode", 1, 32);
    result.wake_mode = ReadString(value, "wakeMode", 1, 32);
    result.microphone = ParseMicrophone(value.at("microphone"));
    if (!value.at("playback").is_boolean() ||
        !value.at("localPreRollMs").is_number_integer() ||
        !value.at("followUpCapture").is_boolean() ||
        !value.at("playbackAcknowledgements").is_boolean()) {
        throw ProtocolError("capability fields have invalid types");
    }
    result.playback = value.at("playback").get<bool>();
    result.local_pre_roll_ms = value.at("localPreRollMs").get<int>();
    result.follow_up_capture = value.at("followUpCapture").get<bool>();
    result.playback_acknowledgements =
        value.at("playbackAcknowledgements").get<bool>();
    result.barge_in_mode = ReadString(value, "bargeInMode", 1, 32);
    ValidateCapabilities(result);
    return result;
}

Health ParseHealth(std::string_view value) {
    if (value == "starting") return Health::Starting;
    if (value == "ready") return Health::Ready;
    if (value == "reconnecting") return Health::Reconnecting;
    if (value == "degraded") return Health::Degraded;
    if (value == "blocked") return Health::Blocked;
    if (value == "muted") return Health::Muted;
    throw ProtocolError("unknown health value");
}

Activity ParseActivity(std::string_view value) {
    if (value == "armed") return Activity::Armed;
    if (value == "wake_pending") return Activity::WakePending;
    if (value == "hearing") return Activity::Hearing;
    if (value == "transcribing") return Activity::Transcribing;
    if (value == "thinking") return Activity::Thinking;
    if (value == "speaking") return Activity::Speaking;
    if (value == "interrupting") return Activity::Interrupting;
    if (value == "follow_up") return Activity::FollowUp;
    if (value == "idle") return Activity::Idle;
    throw ProtocolError("unknown activity value");
}

CancellationSource ParseCancellationSource(std::string_view value) {
    if (value == "physical") return CancellationSource::Physical;
    if (value == "voice") return CancellationSource::Voice;
    if (value == "mute") return CancellationSource::Mute;
    if (value == "session") return CancellationSource::Session;
    throw ProtocolError("unknown cancellation source");
}

Json ParseJson(std::string_view payload) {
    try {
        return Json::parse(payload);
    } catch (...) {
        throw ProtocolError("invalid JSON event");
    }
}

std::string Dump(Json value) {
    return value.dump();
}

Json TurnEvent(std::string_view type, std::string_view turn_id) {
    ValidateIdentifier(turn_id, "turnId");
    return {{"type", type}, {"turnId", turn_id}};
}

}  // namespace

Capabilities ContinuousDeviceCapabilities() {
    return {};
}

void ValidateCapabilities(const Capabilities& capabilities) {
    if (capabilities.endpoint_kind != "halo" &&
        capabilities.endpoint_kind != "device") {
        throw ProtocolError("unsupported endpoint kind");
    }
    if (capabilities.capture_mode != "continuous" &&
        capabilities.capture_mode != "wake_gated") {
        throw ProtocolError("unsupported capture mode");
    }
    if (capabilities.wake_mode != "server" &&
        capabilities.wake_mode != "satellite") {
        throw ProtocolError("unsupported wake mode");
    }
    if (capabilities.microphone != MicrophoneFormat{}) {
        throw ProtocolError("unsupported microphone format");
    }
    if (capabilities.local_pre_roll_ms < 0 ||
        capabilities.local_pre_roll_ms > 5000 ||
        capabilities.local_pre_roll_ms % 20 != 0) {
        throw ProtocolError("invalid local pre-roll");
    }
    if (capabilities.barge_in_mode != "none" &&
        capabilities.barge_in_mode != "wake_word" &&
        capabilities.barge_in_mode != "full_duplex") {
        throw ProtocolError("unsupported barge-in mode");
    }
    if (capabilities.capture_mode == "continuous") {
        if (capabilities.wake_mode != "server" ||
            capabilities.local_pre_roll_ms != 0) {
            throw ProtocolError("continuous capture requires server wake and "
                                "zero local pre-roll");
        }
    } else if (capabilities.wake_mode != "satellite" ||
               capabilities.barge_in_mode == "full_duplex") {
        throw ProtocolError("wake-gated capture requires satellite wake and "
                            "cannot use full-duplex barge-in");
    }
    if (!capabilities.playback &&
        (capabilities.barge_in_mode != "none" ||
         capabilities.playback_acknowledgements)) {
        throw ProtocolError("barge-in and playback acknowledgements require "
                            "playback");
    }
}

std::string SerializeCapabilitiesJson(const Capabilities& capabilities) {
    return Dump(CapabilitiesToJson(capabilities));
}

Capabilities ParseCapabilitiesJson(std::string_view payload) {
    return ParseCapabilities(ParseJson(payload));
}

std::string SerializeSessionAuthenticate(std::string_view ticket) {
    if (ticket.size() < 32 || ticket.size() > 4096) {
        throw ProtocolError("ticket has invalid length");
    }
    return Dump({
        {"type", "session.authenticate"},
        {"protocolVersion", kProtocolVersion},
        {"ticket", ticket},
    });
}

std::string SerializeSessionCapabilities(const Capabilities& capabilities) {
    return Dump({
        {"type", "session.capabilities"},
        {"capabilities", CapabilitiesToJson(capabilities)},
    });
}

std::string SerializeAudioStart(
    const std::optional<std::string>& activation_id) {
    Json value = {
        {"type", "audio.start"},
        {"encoding", "pcm_s16le"},
        {"sampleRate", 16000},
        {"channels", 1},
        {"frameDurationMs", 20},
    };
    if (activation_id.has_value()) {
        ValidateIdentifier(*activation_id, "activationId");
        value["activationId"] = *activation_id;
    }
    return Dump(std::move(value));
}

std::string SerializeWakeManual(
    std::string_view activation_id,
    const std::optional<std::string>& interrupts_turn_id) {
    ValidateIdentifier(activation_id, "activationId");
    Json value = {
        {"type", "wake.manual"},
        {"activationId", activation_id},
    };
    if (interrupts_turn_id.has_value()) {
        ValidateIdentifier(*interrupts_turn_id, "interruptsTurnId");
        value["interruptsTurnId"] = *interrupts_turn_id;
    }
    return Dump(std::move(value));
}

std::string SerializeTurnCancel(const std::optional<std::string>& turn_id,
                                CancellationSource source,
                                std::string_view reason) {
    ValidateReason(reason);
    Json value = {
        {"type", "turn.cancel"},
        {"source", ToString(source)},
        {"reason", reason},
    };
    if (turn_id.has_value()) {
        ValidateIdentifier(*turn_id, "turnId");
        value["turnId"] = *turn_id;
    }
    return Dump(std::move(value));
}

std::string SerializePlaybackStarted(std::string_view turn_id) {
    return Dump(TurnEvent("playback.started", turn_id));
}

std::string SerializePlaybackCompleted(std::string_view turn_id) {
    return Dump(TurnEvent("playback.completed", turn_id));
}

std::string SerializePlaybackStopped(std::string_view turn_id,
                                     std::string_view reason) {
    ValidateReason(reason);
    Json value = TurnEvent("playback.stopped", turn_id);
    value["reason"] = reason;
    return Dump(std::move(value));
}

std::string SerializeMuteChanged(bool muted) {
    return Dump({{"type", "mute.changed"}, {"muted", muted}});
}

std::string SerializeSessionPing(const std::optional<std::string>& nonce) {
    Json value = {{"type", "session.ping"}};
    if (nonce.has_value()) {
        if (nonce->size() > 80) throw ProtocolError("nonce is too long");
        value["nonce"] = *nonce;
    }
    return Dump(std::move(value));
}

ServerEvent ParseServerEvent(std::string_view payload) {
    const Json value = ParseJson(payload);
    if (!value.is_object()) throw ProtocolError("event must be a JSON object");
    const std::string type = ReadString(value, "type", 1, 80);

    if (type == "session.ready") {
        RequireKeys(value,
                    {"type", "sessionId", "protocolVersion", "satellite",
                     "capabilities", "microphone", "health", "activity"});
        if (ReadString(value, "protocolVersion", 1, 8) != kProtocolVersion) {
            throw ProtocolError("unsupported protocol version");
        }
        const Json& satellite = value.at("satellite");
        RequireKeys(satellite, {"satelliteId", "areaId", "label"});
        SessionReady result{
            .session_id = ReadIdentifier(value, "sessionId"),
            .satellite = {
                .satellite_id = ReadIdentifier(satellite, "satelliteId"),
                .area_id = ReadIdentifier(satellite, "areaId"),
                .label = ReadString(satellite, "label", 1, 200),
            },
            .capabilities = ParseCapabilities(value.at("capabilities")),
            .microphone = ParseMicrophone(value.at("microphone")),
            .health = ParseHealth(ReadString(value, "health", 1, 32)),
            .activity = ParseActivity(ReadString(value, "activity", 1, 32)),
        };
        if (result.microphone != result.capabilities.microphone) {
            throw ProtocolError("session microphone does not match capabilities");
        }
        return result;
    }
    if (type == "session.health") {
        RequireKeys(value, {"type", "health", "activity"}, {"nonce"});
        SessionHealth result{
            .health = ParseHealth(ReadString(value, "health", 1, 32)),
            .activity = ParseActivity(ReadString(value, "activity", 1, 32)),
            .nonce = std::nullopt,
        };
        if (value.contains("nonce")) {
            result.nonce = ReadString(value, "nonce", 0, 80);
        }
        return result;
    }
    if (type == "wake.accepted") {
        RequireKeys(value, {"type", "activationId", "turnId"});
        return WakeAccepted{ReadIdentifier(value, "activationId"),
                            ReadIdentifier(value, "turnId")};
    }
    if (type == "wake.suppressed") {
        RequireKeys(value, {"type", "activationId", "reason"});
        return WakeSuppressed{ReadIdentifier(value, "activationId"),
                              ReadString(value, "reason", 1, 200)};
    }
    if (type == "speech.started" || type == "speech.ended" ||
        type == "response.started" || type == "response.completed" ||
        type == "audio.end") {
        RequireKeys(value, {"type", "turnId"});
        const std::string turn_id = ReadIdentifier(value, "turnId");
        if (type == "speech.started") return SpeechStarted{turn_id};
        if (type == "speech.ended") return SpeechEnded{turn_id};
        if (type == "response.started") return ResponseStarted{turn_id};
        if (type == "response.completed") return ResponseCompleted{turn_id};
        return OutputAudioEnd{turn_id};
    }
    if (type == "transcript.final" || type == "response.text") {
        RequireKeys(value, {"type", "turnId", "text"});
        const std::string turn_id = ReadIdentifier(value, "turnId");
        const std::string text = ReadString(value, "text", 0, 1024 * 1024);
        if (type == "transcript.final") return TranscriptFinal{turn_id, text};
        return ResponseText{turn_id, text};
    }
    if (type == "audio.start") {
        RequireKeys(value,
                    {"type", "turnId", "encoding", "sampleRate",
                     "sampleWidth", "channels"});
        if (!value.at("sampleRate").is_number_integer() ||
            !value.at("sampleWidth").is_number_integer() ||
            !value.at("channels").is_number_integer()) {
            throw ProtocolError("output audio format must use integers");
        }
        OutputAudioStart result{
            .turn_id = ReadIdentifier(value, "turnId"),
            .encoding = ReadString(value, "encoding", 1, 32),
            .sample_rate = value.at("sampleRate").get<int>(),
            .sample_width = value.at("sampleWidth").get<int>(),
            .channels = value.at("channels").get<int>(),
        };
        if (result.encoding != "pcm_s16le" || result.sample_width != 2 ||
            result.sample_rate < 8000 || result.sample_rate > 48000 ||
            result.channels < 1 || result.channels > 2) {
            throw ProtocolError("unsupported output audio format");
        }
        return result;
    }
    if (type == "playback.stop") {
        RequireKeys(value, {"type", "turnId", "source", "reason"});
        return PlaybackStop{
            .turn_id = ReadIdentifier(value, "turnId"),
            .source = ParseCancellationSource(
                ReadString(value, "source", 1, 32)),
            .reason = ReadString(value, "reason", 1, 80),
        };
    }
    if (type == "turn.cancelled") {
        RequireKeys(value, {"type"}, {"turnId", "source", "reason"});
        TurnCancelled result;
        if (value.contains("turnId")) {
            result.turn_id = ReadIdentifier(value, "turnId");
        }
        if (value.contains("source")) {
            result.source = ParseCancellationSource(
                ReadString(value, "source", 1, 32));
        }
        if (value.contains("reason")) {
            result.reason = ReadString(value, "reason", 1, 80);
        }
        return result;
    }
    if (type == "error") {
        RequireKeys(value, {"type", "error"});
        const Json& error = value.at("error");
        RequireKeys(error, {"code", "message", "recoverable"});
        if (!error.at("recoverable").is_boolean()) {
            throw ProtocolError("error recoverable field must be boolean");
        }
        return ErrorEvent{
            .code = ReadString(error, "code", 1, 100),
            .message = ReadString(error, "message", 1, 1000),
            .recoverable = error.at("recoverable").get<bool>(),
        };
    }
    throw ProtocolError("unknown server event type: " + type);
}

std::string_view ToString(Health value) {
    switch (value) {
        case Health::Starting: return "starting";
        case Health::Ready: return "ready";
        case Health::Reconnecting: return "reconnecting";
        case Health::Degraded: return "degraded";
        case Health::Blocked: return "blocked";
        case Health::Muted: return "muted";
    }
    return "blocked";
}

std::string_view ToString(Activity value) {
    switch (value) {
        case Activity::Armed: return "armed";
        case Activity::WakePending: return "wake_pending";
        case Activity::Hearing: return "hearing";
        case Activity::Transcribing: return "transcribing";
        case Activity::Thinking: return "thinking";
        case Activity::Speaking: return "speaking";
        case Activity::Interrupting: return "interrupting";
        case Activity::FollowUp: return "follow_up";
        case Activity::Idle: return "idle";
    }
    return "idle";
}

std::string_view ToString(CancellationSource value) {
    switch (value) {
        case CancellationSource::Physical: return "physical";
        case CancellationSource::Voice: return "voice";
        case CancellationSource::Mute: return "mute";
        case CancellationSource::Session: return "session";
    }
    return "session";
}

}  // namespace lva::cortana
