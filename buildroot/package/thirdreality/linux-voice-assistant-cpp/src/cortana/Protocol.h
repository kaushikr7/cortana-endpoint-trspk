#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace lva::cortana {

inline constexpr std::string_view kProtocolVersion = "1";
inline constexpr std::size_t kMicrophoneFrameBytes = 640;

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Health { Starting, Ready, Reconnecting, Degraded, Blocked, Muted };
enum class Activity {
    Armed,
    WakePending,
    Hearing,
    Transcribing,
    Thinking,
    Speaking,
    Interrupting,
    FollowUp,
    Idle,
};
enum class CancellationSource { Physical, Voice, Mute, Session };

struct MicrophoneFormat {
    std::string encoding = "pcm_s16le";
    int sample_rate = 16000;
    int channels = 1;
    int frame_duration_ms = 20;

    bool operator==(const MicrophoneFormat&) const = default;
};

struct Capabilities {
    std::string endpoint_kind = "device";
    std::string capture_mode = "continuous";
    std::string wake_mode = "server";
    MicrophoneFormat microphone;
    bool playback = true;
    int local_pre_roll_ms = 0;
    bool follow_up_capture = true;
    bool playback_acknowledgements = true;
    std::string barge_in_mode = "none";

    bool operator==(const Capabilities&) const = default;
};

struct SatelliteIdentity {
    std::string satellite_id;
    std::string area_id;
    std::string label;
};

struct SessionReady {
    std::string session_id;
    SatelliteIdentity satellite;
    Capabilities capabilities;
    MicrophoneFormat microphone;
    Health health;
    Activity activity;
};
struct SessionHealth {
    Health health;
    Activity activity;
    std::optional<std::string> nonce;
};
struct WakeAccepted { std::string activation_id; std::string turn_id; };
struct WakeSuppressed { std::string activation_id; std::string reason; };
struct SpeechStarted { std::string turn_id; };
struct SpeechEnded { std::string turn_id; };
struct TranscriptFinal { std::string turn_id; std::string text; };
struct ResponseStarted { std::string turn_id; };
struct ResponseText { std::string turn_id; std::string text; };
struct ResponseCompleted { std::string turn_id; };
struct OutputAudioStart {
    std::string turn_id;
    std::string encoding;
    int sample_rate;
    int sample_width;
    int channels;
};
struct OutputAudioEnd { std::string turn_id; };
struct PlaybackStop {
    std::string turn_id;
    CancellationSource source;
    std::string reason;
};
struct TurnCancelled {
    std::optional<std::string> turn_id;
    std::optional<CancellationSource> source;
    std::optional<std::string> reason;
};
struct ErrorEvent {
    std::string code;
    std::string message;
    bool recoverable;
};

using ServerEvent = std::variant<
    SessionReady,
    SessionHealth,
    WakeAccepted,
    WakeSuppressed,
    SpeechStarted,
    SpeechEnded,
    TranscriptFinal,
    ResponseStarted,
    ResponseText,
    ResponseCompleted,
    OutputAudioStart,
    OutputAudioEnd,
    PlaybackStop,
    TurnCancelled,
    ErrorEvent>;

Capabilities ContinuousDeviceCapabilities();
void ValidateCapabilities(const Capabilities& capabilities);
std::string SerializeCapabilitiesJson(const Capabilities& capabilities);
Capabilities ParseCapabilitiesJson(std::string_view payload);

std::string SerializeSessionAuthenticate(std::string_view ticket);
std::string SerializeSessionCapabilities(const Capabilities& capabilities);
std::string SerializeAudioStart(
    const std::optional<std::string>& activation_id = std::nullopt);
std::string SerializeWakeManual(
    std::string_view activation_id,
    const std::optional<std::string>& interrupts_turn_id = std::nullopt);
std::string SerializeTurnCancel(
    const std::optional<std::string>& turn_id,
    CancellationSource source = CancellationSource::Physical,
    std::string_view reason = "user_cancelled");
std::string SerializePlaybackStarted(std::string_view turn_id);
std::string SerializePlaybackCompleted(std::string_view turn_id);
std::string SerializePlaybackStopped(
    std::string_view turn_id,
    std::string_view reason = "user_stopped");
std::string SerializeMuteChanged(bool muted);
std::string SerializeSessionPing(
    const std::optional<std::string>& nonce = std::nullopt);

ServerEvent ParseServerEvent(std::string_view payload);

std::string_view ToString(Health value);
std::string_view ToString(Activity value);
std::string_view ToString(CancellationSource value);

}  // namespace lva::cortana
