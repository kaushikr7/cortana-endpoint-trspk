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
#include "config/EndpointConfig.h"
#include "cortana/CurlSessionTransport.h"
#include "cortana/Protocol.h"
#include "cortana/SessionClient.h"
#include "tr/HomeButton.h"
#include "tr/LedController.h"
#include "tr/MicMuteGpio.h"
#include "tr/PhysicalControlPolicy.h"
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

lva::tr::EndpointActivity EndpointActivityFor(
    const lva::cortana::SessionSnapshot& snapshot) {
    if (snapshot.phase != lva::cortana::SessionPhase::Ready) {
        return lva::tr::EndpointActivity::Unavailable;
    }
    switch (snapshot.activity) {
        case lva::cortana::Activity::Armed:
        case lva::cortana::Activity::Idle:
            return lva::tr::EndpointActivity::Armed;
        case lva::cortana::Activity::Speaking:
            return lva::tr::EndpointActivity::Playback;
        default:
            return lva::tr::EndpointActivity::ActiveTurn;
    }
}

void ApplyLedState(const lva::cortana::SessionSnapshot& snapshot,
                   lva::tr::LedController& leds) {
    using lva::cortana::Activity;
    using lva::cortana::SessionPhase;
    using lva::tr::LedState;

    leds.SetBlocked(snapshot.phase == SessionPhase::Blocked);
    if (snapshot.phase == SessionPhase::Connecting ||
        snapshot.phase == SessionPhase::Negotiating) {
        leds.SetConnection(snapshot.generation <= 1
                               ? LedState::Booting
                               : LedState::Reconnecting);
        leds.ClearTurn();
        return;
    }
    if (snapshot.phase == SessionPhase::Backoff) {
        leds.SetConnection(LedState::Reconnecting);
        leds.ClearTurn();
        return;
    }
    if (snapshot.phase != SessionPhase::Ready) {
        leds.ClearConnection();
        leds.ClearTurn();
        return;
    }

    leds.ClearConnection();
    switch (snapshot.activity) {
        case Activity::WakePending:
        case Activity::Hearing:
        case Activity::FollowUp:
            leds.SetTurn(LedState::Listening);
            break;
        case Activity::Transcribing:
        case Activity::Thinking:
            leds.SetTurn(LedState::Thinking);
            break;
        case Activity::Speaking:
        case Activity::Interrupting:
            leds.SetTurn(LedState::Speaking);
            break;
        case Activity::Armed:
        case Activity::Idle:
            leds.ClearTurn();
            break;
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

    lva::audio::CapturePipeline::Options capture_options;
    capture_options.capture.alsa_device = cli.capture_alsa_device;
    capture_options.capture.mic_channel = cli.capture_mic_channel;
    capture_options.capture.ref_channels = cli.capture_ref_channels;
    lva::audio::CapturePipeline capture(std::move(capture_options));
    if (!capture.Start()) {
        LVA_LOGE(kTag, "%s", "continuous microphone capture is unavailable");
        leds.SetSystem(lva::tr::LedState::Error);
    }

    bool muted = false;
    lva::tr::MicMuteGpio mute_gpio(
        [&session, &capture, &muted](
            bool new_muted, lva::tr::MuteChangeSource) {
            muted = new_muted;
            if (new_muted) (void)capture.DiscardQueued();
            (void)session.EnqueueText(
                lva::cortana::SerializeMuteChanged(new_muted));
        });
    if (mute_gpio.Available()) (void)mute_gpio.ReadAndApplyOnce();

    std::uint64_t activation_sequence = 0;
    lva::tr::HomeButton home_button(
        lva::tr::HomeButton::Options{},
        [&session, &activation_sequence](lva::tr::HomeButtonPress press) {
            const auto snapshot = session.Snapshot();
            const auto action = lva::tr::PhysicalControlPolicy::OnHomeButton(
                press, EndpointActivityFor(snapshot));
            if (action == lva::tr::ControlAction::ManualWake) {
                (void)session.EnqueueText(lva::cortana::SerializeWakeManual(
                    NewManualActivationId(++activation_sequence)));
            } else if (action == lva::tr::ControlAction::CancelTurn) {
                (void)session.EnqueueText(lva::cortana::SerializeTurnCancel(
                    std::nullopt, lva::cortana::CancellationSource::Physical,
                    "user_cancelled"));
            }
        });
    const int home_button_fd = home_button.Start();
    if (home_button_fd < 0) {
        LVA_LOGW(kTag, "%s", "home button monitor disabled");
    }

    lva::cortana::SessionPhase logged_phase =
        lva::cortana::SessionPhase::Stopped;
    std::uint64_t mute_synced_generation = 0;
    auto next_capture_metrics = std::chrono::steady_clock::now() + 30s;
    while (g_shutdown_signal.load(std::memory_order_relaxed) == 0) {
        if (home_button_fd >= 0) {
            pollfd descriptor{
                .fd = home_button_fd,
                .events = POLLIN,
                .revents = 0,
            };
            const int result = ::poll(&descriptor, 1, 50);
            if (result > 0 && (descriptor.revents & POLLIN) != 0) {
                home_button.OnMainLoopWake();
            } else if (result < 0 && errno != EINTR) {
                LVA_LOGW(kTag, "home button poll failed: %s",
                         std::strerror(errno));
            }
        } else {
            std::this_thread::sleep_for(50ms);
        }

        mute_gpio.Poll();
        leds.Poll();
        // T3.2 replaces this deliberate drain with generation-aware 20 ms
        // WSS frame transport. Never retain stale microphone latency.
        (void)capture.DiscardQueued();
        while (session.TryPopEvent().has_value()) {
        }

        const auto snapshot = session.Snapshot();
        ApplyLedState(snapshot, leds);
        if (snapshot.phase != logged_phase) {
            logged_phase = snapshot.phase;
            LVA_LOGI(kTag, "Cortana session phase=%.*s generation=%llu%s%s",
                     static_cast<int>(lva::cortana::ToString(snapshot.phase).size()),
                     lva::cortana::ToString(snapshot.phase).data(),
                     static_cast<unsigned long long>(snapshot.generation),
                     snapshot.detail.empty() ? "" : " detail=",
                     snapshot.detail.c_str());
        }
        if (snapshot.phase == lva::cortana::SessionPhase::Ready &&
            mute_synced_generation != snapshot.generation &&
            session.EnqueueText(lva::cortana::SerializeMuteChanged(muted))) {
            mute_synced_generation = snapshot.generation;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_capture_metrics) {
            next_capture_metrics = now + 30s;
            const auto metrics = capture.GetMetrics();
            LVA_LOGI(kTag,
                     "capture running=%d periods=%llu samples=%llu "
                     "reference_periods=%llu queue=%zu/%zu high=%zu "
                     "overrun=%llu discarded=%llu recoveries=%llu "
                     "short_reads=%llu processing_failures=%llu max_us=%llu",
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
                         metrics.maximum_processing_us));
        }
    }

    home_button.Stop();
    capture.Stop();
    session.Stop();
    const int signal = g_shutdown_signal.load(std::memory_order_relaxed);
    LVA_LOGI(kTag, "exiting after signal %d", signal);
    return 0;
}
