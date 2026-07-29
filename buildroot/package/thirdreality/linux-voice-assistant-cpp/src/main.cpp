#include <getopt.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
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
#include <thread>

#include <nlohmann/json.hpp>

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
    bool debug = false;
};

bool ParseCli(int argc, char** argv, CliOptions& output) {
    constexpr int kOptCheckConfig = 1000;
    constexpr int kOptStatus = 1001;
    constexpr int kOptConfigFile = 1002;
    constexpr int kOptCredentialFile = 1003;
    static const option kOptions[] = {
        {"check-config", no_argument, nullptr, kOptCheckConfig},
        {"status", no_argument, nullptr, kOptStatus},
        {"config-file", required_argument, nullptr, kOptConfigFile},
        {"credential-file", required_argument, nullptr, kOptCredentialFile},
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

    bool muted = false;
    lva::tr::MicMuteGpio mute_gpio(
        [&session, &muted](bool new_muted, lva::tr::MuteChangeSource) {
            muted = new_muted;
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
    }

    home_button.Stop();
    session.Stop();
    const int signal = g_shutdown_signal.load(std::memory_order_relaxed);
    LVA_LOGI(kTag, "exiting after signal %d", signal);
    return 0;
}
