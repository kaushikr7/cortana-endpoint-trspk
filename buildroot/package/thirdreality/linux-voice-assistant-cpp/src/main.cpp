#include <getopt.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "audio/CapturePipeline.h"
#include "audio/CaptureSupervisor.h"
#include "audio/MicrophoneIngress.h"
#include "audio/RawPcmPlayer.h"
#include "config/EndpointConfig.h"
#include "cortana/CurlSessionTransport.h"
#include "cortana/EndpointRuntime.h"
#include "cortana/Protocol.h"
#include "cortana/SessionClient.h"
#include "tr/EndpointLedPolicy.h"
#include "tr/HomeButton.h"
#include "tr/LedController.h"
#include "tr/MicMuteGpio.h"
#include "util/Log.h"

namespace {

using namespace std::chrono_literals;

constexpr const char* kTag = "main";
constexpr const char* kVersion = "0.0.1";

std::atomic<int> g_shutdown_signal{0};

extern "C" void OnSignal(int signo) {
    g_shutdown_signal.store(signo, std::memory_order_relaxed);
}

extern "C" void OnSigchld(int /*signo*/) {
    const int saved_errno = errno;
    while (::waitpid(-1, nullptr, WNOHANG) > 0) {
    }
    errno = saved_errno;
}

void InstallSignalHandlers() {
    struct sigaction action {};
    action.sa_handler = &OnSignal;
    ::sigemptyset(&action.sa_mask);
    ::sigaction(SIGTERM, &action, nullptr);
    ::sigaction(SIGINT, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    struct sigaction child_action {};
    child_action.sa_handler = &OnSigchld;
    ::sigemptyset(&child_action.sa_mask);
    child_action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    ::sigaction(SIGCHLD, &child_action, nullptr);
}

void PrintUsage(const char* argv0) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --check-config          Validate Cortana config and credential\n"
        "  --status                Print redacted Cortana config status as JSON\n"
        "  --config-file <p>       Cortana config file\n"
        "                           (default: /data/cortana/config.json)\n"
        "  --credential-file <p>   Cortana credential file\n"
        "                           (default: /data/cortana/credential)\n"
        "  --capture-alsa-device <d>\n"
        "                           ALSA capture device (default: hw:0,4)\n"
        "  --capture-mic-channel <n>\n"
        "                           0-based microphone channel (default: 0)\n"
        "  --capture-ref-channels <r>\n"
        "                           none, one, or two AEC channels (default: 2,3)\n"
        "  --capture-gain-db <n>   AGC2 fixed digital gain, 0-49 "
        "(default: 42)\n"
        "  --debug                 Enable debug logging\n"
        "  --help                  Show this help and exit\n",
        argv0);
}

struct CliOptions {
    enum class ConfigCommand { None, Check, Status };

    ConfigCommand config_command = ConfigCommand::None;
    std::filesystem::path config_file = lva::config::kDefaultConfigPath;
    std::filesystem::path credential_file =
        lva::config::kDefaultCredentialPath;
    std::string capture_alsa_device = "hw:0,4";
    unsigned capture_mic_channel = 0;
    std::array<int, 2> capture_ref_channels = {2, 3};
    unsigned capture_gain_db = static_cast<unsigned>(
        lva::audio::WebRtcProcessor::kDefaultGainDb);
    bool debug = false;
};

bool ParseUnsigned(std::string_view text, unsigned* output) {
    if (text.empty()) return false;
    unsigned value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        value = value * 10 + static_cast<unsigned>(character - '0');
        if (value > 255) return false;
    }
    *output = value;
    return true;
}

bool ParseReferenceChannels(std::string_view text,
                            std::array<int, 2>* output) {
    if (text.empty() || text == "none") {
        *output = {-1, -1};
        return true;
    }
    const std::size_t comma = text.find(',');
    if (comma != std::string_view::npos &&
        text.find(',', comma + 1) != std::string_view::npos) {
        return false;
    }
    unsigned first = 0;
    unsigned second = 0;
    if (!ParseUnsigned(text.substr(0, comma), &first)) return false;
    if (comma == std::string_view::npos) {
        *output = {static_cast<int>(first), -1};
        return true;
    }
    if (!ParseUnsigned(text.substr(comma + 1), &second)) return false;
    *output = {static_cast<int>(first), static_cast<int>(second)};
    return true;
}

bool ParseCli(int argc, char** argv, CliOptions& output) {
    constexpr int kOptCheckConfig = 1000;
    constexpr int kOptStatus = 1001;
    constexpr int kOptConfigFile = 1002;
    constexpr int kOptCredentialFile = 1003;
    constexpr int kOptCaptureAlsaDevice = 1004;
    constexpr int kOptCaptureMicChannel = 1005;
    constexpr int kOptCaptureRefChannels = 1006;
    constexpr int kOptCaptureGainDb = 1007;
    static const option kOptions[] = {
        {"check-config", no_argument, nullptr, kOptCheckConfig},
        {"status", no_argument, nullptr, kOptStatus},
        {"config-file", required_argument, nullptr, kOptConfigFile},
        {"credential-file", required_argument, nullptr, kOptCredentialFile},
        {"capture-alsa-device", required_argument, nullptr,
         kOptCaptureAlsaDevice},
        {"capture-mic-channel", required_argument, nullptr,
         kOptCaptureMicChannel},
        {"capture-ref-channels", required_argument, nullptr,
         kOptCaptureRefChannels},
        {"capture-gain-db", required_argument, nullptr, kOptCaptureGainDb},
        {"debug", no_argument, nullptr, 'd'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int option_value = 0;
    while ((option_value =
                ::getopt_long(argc, argv, "", kOptions, nullptr)) != -1) {
        switch (option_value) {
            case kOptCheckConfig:
                if (output.config_command != CliOptions::ConfigCommand::None) {
                    std::fprintf(
                        stderr, "Choose only one of --check-config or --status\n");
                    return false;
                }
                output.config_command = CliOptions::ConfigCommand::Check;
                break;
            case kOptStatus:
                if (output.config_command != CliOptions::ConfigCommand::None) {
                    std::fprintf(
                        stderr, "Choose only one of --check-config or --status\n");
                    return false;
                }
                output.config_command = CliOptions::ConfigCommand::Status;
                break;
            case kOptConfigFile: output.config_file = optarg; break;
            case kOptCredentialFile: output.credential_file = optarg; break;
            case kOptCaptureAlsaDevice:
                output.capture_alsa_device = optarg;
                break;
            case kOptCaptureMicChannel:
                if (!ParseUnsigned(optarg, &output.capture_mic_channel)) {
                    std::fprintf(stderr, "Invalid capture microphone channel\n");
                    return false;
                }
                break;
            case kOptCaptureRefChannels:
                if (!ParseReferenceChannels(
                        optarg, &output.capture_ref_channels)) {
                    std::fprintf(stderr, "Invalid capture reference channels\n");
                    return false;
                }
                break;
            case kOptCaptureGainDb:
                if (!ParseUnsigned(optarg, &output.capture_gain_db) ||
                    output.capture_gain_db >
                        static_cast<unsigned>(
                            lva::audio::WebRtcProcessor::kMaximumGainDb)) {
                    std::fprintf(stderr, "Invalid capture gain dB\n");
                    return false;
                }
                break;
            case 'd': output.debug = true; break;
            case 'h':
                PrintUsage(argv[0]);
                std::exit(0);
            default:
                PrintUsage(argv[0]);
                return false;
        }
    }
    if (optind != argc) {
        std::fprintf(stderr, "Unexpected positional argument: %s\n",
                     argv[optind]);
        return false;
    }
    return true;
}

int RunConfigCommand(const CliOptions& cli) {
    try {
        const auto config = lva::config::EndpointConfig::Load(
            cli.config_file, cli.credential_file);
        if (cli.config_command == CliOptions::ConfigCommand::Status) {
            std::printf("%s\n", config.RedactedStatusJson().c_str());
        } else {
            std::printf("Cortana endpoint configuration is valid "
                        "(satelliteId=%s, endpoint=%s",
                        config.satellite_id.c_str(), config.endpoint.c_str());
            if (!config.expected_area_id.empty()) {
                std::printf(", expectedAreaId=%s",
                            config.expected_area_id.c_str());
            }
            std::printf(")\n");
        }
        return 0;
    } catch (const lva::config::ConfigError& error) {
        if (cli.config_command == CliOptions::ConfigCommand::Status) {
            const nlohmann::json status = {
                {"configured", false},
                {"error", error.what()},
            };
            std::printf("%s\n", status.dump().c_str());
        } else {
            std::fprintf(stderr, "Cortana configuration invalid: %s\n",
                         error.what());
        }
        return 1;
    }
}

void WaitForShutdown() {
    while (g_shutdown_signal.load(std::memory_order_relaxed) == 0) {
        std::this_thread::sleep_for(100ms);
    }
}

std::string NewManualActivationId(std::uint64_t sequence) {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return "manual-" + std::to_string(milliseconds) + "-" +
        std::to_string(sequence);
}

const char* PlaybackStateName(lva::audio::RawPlaybackState state) {
    switch (state) {
        case lva::audio::RawPlaybackState::Idle: return "idle";
        case lva::audio::RawPlaybackState::Playing: return "playing";
        case lva::audio::RawPlaybackState::Draining: return "draining";
        case lva::audio::RawPlaybackState::Error: return "error";
    }
    return "error";
}

}  // namespace

int main(int argc, char** argv) {
    CliOptions cli;
    if (!ParseCli(argc, argv, cli)) return 2;
    if (cli.config_command != CliOptions::ConfigCommand::None) {
        return RunConfigCommand(cli);
    }

    lva::log::SetLevel(cli.debug ? lva::log::Level::kDebug
                                 : lva::log::Level::kInfo);
    InstallSignalHandlers();

    lva::tr::LedController leds;
    leds.SetBase(lva::tr::LedState::Ready);
    leds.SetConnection(lva::tr::LedState::Booting);

    lva::config::EndpointConfig config;
    try {
        config = lva::config::EndpointConfig::Load(
            cli.config_file, cli.credential_file);
    } catch (const lva::config::ConfigError& error) {
        LVA_LOGE(kTag, "Cortana endpoint configuration is blocked: %s",
                 error.what());
        leds.SetBlocked(true);
        WaitForShutdown();
        return 1;
    }

    LVA_LOGI(kTag,
             "linux-voice-assistant-cpp %s starting "
             "(satellite_id=%s endpoint=%s)",
             kVersion, config.satellite_id.c_str(), config.endpoint.c_str());

    std::shared_ptr<lva::cortana::SessionDependencies> dependencies;
    try {
        dependencies =
            std::make_shared<lva::cortana::CurlSessionDependencies>();
    } catch (const std::exception& error) {
        LVA_LOGE(kTag, "Cortana transport initialization is blocked: %s",
                 error.what());
        leds.SetBlocked(true);
        WaitForShutdown();
        return 1;
    }

    lva::cortana::SessionClient session(config, std::move(dependencies));
    session.Start();

    lva::audio::RawPcmPlayer player(
        lva::audio::RawPcmPlayer::Options{},
        [] {
            // This is the PulseAudio sink backed by ALSA hw:0,1. The codec's
            // hardware loopback exposes the same output on capture channels
            // 2/3 for AEC.
            return lva::audio::MakePulseAudioSink(
                "alsa_output.hw_0_1");
        });
    lva::audio::CapturePipeline::Options capture_options;
    capture_options.capture.alsa_device = cli.capture_alsa_device;
    capture_options.capture.mic_channel = cli.capture_mic_channel;
    capture_options.capture.ref_channels = cli.capture_ref_channels;
    capture_options.automatic_gain_db =
        static_cast<int>(cli.capture_gain_db);
    lva::audio::CapturePipeline capture(std::move(capture_options));
    lva::audio::CaptureSupervisor capture_supervisor({
        .start = [&capture] { return capture.Start(); },
        .stop = [&capture] { capture.Stop(); },
        .flush_audio = [&capture, &session] {
            (void)capture.DiscardQueued();
            capture.ResetProcessing();
            session.DiscardAudioFrames();
        },
        .metrics = [&capture] { return capture.GetMetrics(); },
    });
    capture_supervisor.Start();
    lva::audio::MicrophoneIngress ingress(
        capture.Queue(),
        [&session](
            std::uint64_t generation,
            const lva::audio::MicrophoneIngress::Frame& frame) {
            if (session.EnqueueAudioFrame(generation, frame)) return true;
            session.DiscardAudioFrames();
            return false;
        });

    std::uint64_t activation_sequence = 0;
    lva::cortana::EndpointRuntime runtime({
        .send_command = [&session](std::string payload) {
            return session.EnqueueText(std::move(payload));
        },
        .set_microphone_muted = [&session](bool muted) {
            session.SetMicrophoneMuted(muted);
        },
        .discard_capture = [&capture] {
            (void)capture.DiscardQueued();
        },
        .begin_playback = [&player](
            std::string turn_id, lva::audio::PcmFormat format) {
            return player.Begin(std::move(turn_id), std::move(format));
        },
        .enqueue_playback = [&player](
            std::string_view turn_id, std::string payload) {
            return player.Enqueue(turn_id, std::move(payload));
        },
        .end_playback = [&player](std::string_view turn_id) {
            return player.End(turn_id);
        },
        .stop_playback = [&player](std::string turn_id, std::string reason) {
            player.Stop(std::move(turn_id), std::move(reason));
        },
        .new_activation_id = [&activation_sequence] {
            return NewManualActivationId(++activation_sequence);
        },
    });

    lva::tr::MicMuteGpio mute_gpio(
        [&runtime, &capture_supervisor](
            bool new_muted, lva::tr::MuteChangeSource source) {
            runtime.OnMuteChanged(new_muted);
            if (!new_muted &&
                source == lva::tr::MuteChangeSource::Hardware) {
                capture_supervisor.RestartAfterHardwareChange();
            }
        });
    if (mute_gpio.Available()) (void)mute_gpio.ReadAndApplyOnce();

    lva::tr::HomeButton home_button(
        lva::tr::HomeButton::Options{},
        [&runtime](lva::tr::HomeButtonPress press) {
            runtime.OnHomeButton(press);
        });
    const int home_button_fd = home_button.Start();
    if (home_button_fd < 0) {
        LVA_LOGW(kTag, "%s", "home button monitor disabled");
    }

    lva::cortana::SessionPhase logged_phase =
        lva::cortana::SessionPhase::Stopped;
    auto next_capture_metrics = std::chrono::steady_clock::now() + 30s;
    while (g_shutdown_signal.load(std::memory_order_relaxed) == 0) {
        if (home_button_fd >= 0) {
            pollfd descriptor{
                .fd = home_button_fd,
                .events = POLLIN,
                .revents = 0,
            };
            const int result = ::poll(&descriptor, 1, 10);
            if (result > 0 && (descriptor.revents & POLLIN) != 0) {
                home_button.OnMainLoopWake();
            } else if (result < 0 && errno != EINTR) {
                LVA_LOGW(kTag, "home button poll failed: %s",
                         std::strerror(errno));
            }
        } else {
            std::this_thread::sleep_for(10ms);
        }

        mute_gpio.Poll();
        leds.Poll();
        auto snapshot = session.Snapshot();
        runtime.UpdateSession(snapshot);
        while (const auto event = session.TryPopEvent()) {
            snapshot = session.Snapshot();
            runtime.UpdateSession(snapshot);
            runtime.HandleServerEvent(*event);
        }

        snapshot = session.Snapshot();
        runtime.UpdateSession(snapshot);
        while (const auto result = player.TryPopResult()) {
            const char* outcome = "error";
            if (result->outcome == lva::audio::RawPlaybackOutcome::Started) {
                outcome = "started";
            } else if (result->outcome ==
                lva::audio::RawPlaybackOutcome::Completed) {
                outcome = "completed";
            } else if (result->outcome ==
                       lva::audio::RawPlaybackOutcome::Stopped) {
                outcome = "stopped";
            }
            runtime.HandlePlaybackResult(*result);
            LVA_LOGI(kTag, "playback turn=%s outcome=%s%s%s",
                     result->turn_id.c_str(), outcome,
                     result->detail.empty() ? "" : " detail=",
                     result->detail.c_str());
        }
        runtime.PumpCommands();
        const auto endpoint = runtime.Snapshot();
        const auto now = std::chrono::steady_clock::now();
        capture_supervisor.Poll(now);
        const auto capture_status = capture_supervisor.Snapshot(now);
        (void)ingress.Pump(
            snapshot,
            endpoint.muted ||
                capture_status.state !=
                    lva::audio::CaptureLifecycleState::Ready);
        lva::tr::EndpointLedPolicy::Apply(
            endpoint, capture_status.state, leds);
        if (snapshot.phase != logged_phase) {
            logged_phase = snapshot.phase;
            LVA_LOGI(kTag, "Cortana session phase=%.*s generation=%llu%s%s",
                     static_cast<int>(lva::cortana::ToString(snapshot.phase).size()),
                     lva::cortana::ToString(snapshot.phase).data(),
                     static_cast<unsigned long long>(snapshot.generation),
                     snapshot.detail.empty() ? "" : " detail=",
                     snapshot.detail.c_str());
        }
        if (now >= next_capture_metrics) {
            next_capture_metrics = now + 30s;
            const auto metrics = capture.GetMetrics();
            const auto ingress_metrics = ingress.GetMetrics();
            const auto playback_metrics = player.Snapshot();
            const auto runtime_metrics = runtime.Metrics();
            LVA_LOGI(kTag,
                     "capture running=%d periods=%llu samples=%llu "
                     "reference_periods=%llu queue=%zu/%zu high=%zu "
                     "overrun=%llu discarded=%llu recoveries=%llu "
                     "short_reads=%llu processing_failures=%llu max_us=%llu "
                     "frames_enqueued=%llu frames_rejected=%llu "
                     "session_queue=%zu session_sent=%llu session_dropped=%llu "
                     "overload_reconnects=%llu generation=%llu "
                     "playback_state=%s playback_queue=%zu playback_high=%zu "
                     "playback_written=%llu playback_rejected=%llu "
                     "playback_discarded=%llu playback_errors=%llu "
                     "runtime_queue=%zu runtime_sent=%llu runtime_dropped=%llu "
                     "capture_state=%.*s capture_failure=%.*s "
                     "capture_attempts=%llu capture_boundaries=%llu "
                     "capture_planned_restarts=%llu "
                     "capture_exits=%llu capture_stalls=%llu "
                     "capture_consecutive_failures=%u capture_retry_ms=%llu",
                     metrics.running ? 1 : 0,
                     static_cast<unsigned long long>(metrics.periods_captured),
                     static_cast<unsigned long long>(metrics.samples_captured),
                     static_cast<unsigned long long>(metrics.reference_periods),
                     metrics.queue.queued_samples,
                     metrics.queue.capacity_samples,
                     metrics.queue.high_watermark_samples,
                     static_cast<unsigned long long>(
                         metrics.queue.samples_dropped),
                     static_cast<unsigned long long>(
                         metrics.queue.samples_discarded),
                     static_cast<unsigned long long>(metrics.recoveries),
                     static_cast<unsigned long long>(metrics.short_reads),
                     static_cast<unsigned long long>(
                         metrics.processing_failures),
                     static_cast<unsigned long long>(
                         metrics.maximum_processing_us),
                     static_cast<unsigned long long>(
                         ingress_metrics.frames_enqueued),
                     static_cast<unsigned long long>(
                         ingress_metrics.frames_rejected),
                     snapshot.queued_audio_frames,
                     static_cast<unsigned long long>(
                         snapshot.audio_frames_sent),
                     static_cast<unsigned long long>(
                         snapshot.dropped_audio_frames),
                     static_cast<unsigned long long>(
                         snapshot.audio_overload_reconnects),
                     static_cast<unsigned long long>(snapshot.generation),
                     PlaybackStateName(playback_metrics.state),
                     playback_metrics.queued_bytes,
                     playback_metrics.queue_high_watermark,
                     static_cast<unsigned long long>(
                         playback_metrics.bytes_written),
                     static_cast<unsigned long long>(
                         playback_metrics.bytes_rejected),
                     static_cast<unsigned long long>(
                         playback_metrics.bytes_discarded),
                     static_cast<unsigned long long>(
                         playback_metrics.errors),
                     runtime_metrics.queued_commands,
                     static_cast<unsigned long long>(
                         runtime_metrics.commands_sent),
                     static_cast<unsigned long long>(
                         runtime_metrics.commands_dropped),
                     static_cast<int>(
                         lva::audio::ToString(capture_status.state).size()),
                     lva::audio::ToString(capture_status.state).data(),
                     static_cast<int>(
                         lva::audio::ToString(
                             capture_status.last_failure).size()),
                     lva::audio::ToString(
                         capture_status.last_failure).data(),
                     static_cast<unsigned long long>(
                         capture_status.start_attempts),
                     static_cast<unsigned long long>(
                         capture_status.recovery_boundaries),
                     static_cast<unsigned long long>(
                         capture_status.planned_restarts),
                     static_cast<unsigned long long>(
                         capture_status.exited_workers),
                     static_cast<unsigned long long>(
                         capture_status.stalled_workers),
                     capture_status.consecutive_failures,
                     static_cast<unsigned long long>(
                         capture_status.retry_in_ms));
        }
    }

    home_button.Stop();
    capture_supervisor.Stop();
    session.Stop();
    const int signal = g_shutdown_signal.load(std::memory_order_relaxed);
    LVA_LOGI(kTag, "exiting after signal %d", signal);
    return 0;
}
