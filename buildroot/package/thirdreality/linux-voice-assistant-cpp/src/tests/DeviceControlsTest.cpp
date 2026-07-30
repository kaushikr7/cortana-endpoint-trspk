#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "tr/HomeButton.h"
#include "tr/EndpointLedPolicy.h"
#include "tr/LedController.h"
#include "tr/MicMuteGpio.h"
#include "tr/PhysicalControlPolicy.h"

namespace {

using lva::tr::LedState;

void TestLedPriorityAndExpiry() {
    std::vector<std::pair<std::string, bool>> rendered;
    lva::tr::LedController leds(
        [&rendered](const lva::tr::LedPresentation& presentation) {
            rendered.emplace_back(std::string(presentation.animation),
                                  presentation.return_to_idle);
        });

    leds.SetBase(LedState::Ready);
    assert(rendered.back() == std::make_pair(std::string("none.animation"), true));

    const auto start = lva::tr::LedController::Clock::time_point{};
    leds.ShowVolumeChanged(std::chrono::milliseconds(100), start);
    assert(leds.EffectiveState() == LedState::Volume);

    leds.SetConnection(LedState::Reconnecting);
    assert(leds.EffectiveState() == LedState::Reconnecting);
    assert(rendered.back().first == "alert-short.animation");

    leds.SetTurn(LedState::Listening);
    assert(leds.EffectiveState() == LedState::Listening);
    leds.SetBlocked(true);
    assert(leds.EffectiveState() == LedState::Blocked);
    assert(rendered.back().first == "red.animation");

    leds.Poll(start + std::chrono::milliseconds(101));
    assert(leds.EffectiveState() == LedState::Blocked);
    leds.SetBlocked(false);
    assert(leds.EffectiveState() == LedState::Listening);
    leds.ClearTurn();
    assert(leds.EffectiveState() == LedState::Reconnecting);
    leds.ClearConnection();
    assert(leds.EffectiveState() == LedState::Ready);

    const auto error = lva::tr::LedController::PresentationFor(LedState::Error);
    assert(error.animation == "error.animation");
    assert(error.return_to_idle);
}

void TestEndpointLedStateMapping() {
    lva::tr::LedController leds([](const lva::tr::LedPresentation&) {});
    leds.SetBase(LedState::Ready);
    lva::cortana::EndpointSnapshot endpoint;
    endpoint.phase = lva::cortana::SessionPhase::Connecting;
    endpoint.generation = 1;
    lva::tr::EndpointLedPolicy::Apply(endpoint, leds);
    assert(leds.EffectiveState() == LedState::Booting);

    endpoint.phase = lva::cortana::SessionPhase::Backoff;
    endpoint.generation = 2;
    lva::tr::EndpointLedPolicy::Apply(endpoint, leds);
    assert(leds.EffectiveState() == LedState::Reconnecting);

    endpoint.phase = lva::cortana::SessionPhase::Ready;
    endpoint.activity = lva::cortana::Activity::Hearing;
    lva::tr::EndpointLedPolicy::Apply(endpoint, leds);
    assert(leds.EffectiveState() == LedState::Listening);
    endpoint.activity = lva::cortana::Activity::Thinking;
    lva::tr::EndpointLedPolicy::Apply(endpoint, leds);
    assert(leds.EffectiveState() == LedState::Thinking);
    endpoint.activity = lva::cortana::Activity::Speaking;
    lva::tr::EndpointLedPolicy::Apply(endpoint, leds);
    assert(leds.EffectiveState() == LedState::Speaking);
    endpoint.activity = lva::cortana::Activity::Interrupting;
    lva::tr::EndpointLedPolicy::Apply(endpoint, leds);
    assert(leds.EffectiveState() == LedState::Cancelling);

    // Mute has a dedicated button LED and must not claim the ring.
    endpoint.muted = true;
    endpoint.activity = lva::cortana::Activity::Armed;
    lva::tr::EndpointLedPolicy::Apply(endpoint, leds);
    assert(leds.EffectiveState() == LedState::Ready);

    endpoint.phase = lva::cortana::SessionPhase::Blocked;
    lva::tr::EndpointLedPolicy::Apply(endpoint, leds);
    assert(leds.EffectiveState() == LedState::Blocked);

    endpoint.phase = lva::cortana::SessionPhase::Ready;
    lva::tr::EndpointLedPolicy::Apply(
        endpoint, lva::audio::CaptureLifecycleState::Degraded, leds);
    assert(leds.EffectiveState() == LedState::Reconnecting);
    lva::tr::EndpointLedPolicy::Apply(
        endpoint, lva::audio::CaptureLifecycleState::Blocked, leds);
    assert(leds.EffectiveState() == LedState::Error);
    lva::tr::EndpointLedPolicy::Apply(
        endpoint, lva::audio::CaptureLifecycleState::Ready, leds);
    assert(leds.EffectiveState() == LedState::Ready);
}

void TestHomeButtonClassification() {
    using lva::tr::HomeButton;
    using lva::tr::HomeButtonPress;
    assert(HomeButton::ClassifyClicks(1) == HomeButtonPress::Single);
    assert(HomeButton::ClassifyClicks(2) == HomeButtonPress::Double);
    assert(HomeButton::ClassifyClicks(3) == HomeButtonPress::Triple);
    assert(HomeButton::ClassifyClicks(9) == HomeButtonPress::Triple);
}

void TestPhysicalControlPolicy() {
    using lva::tr::ControlAction;
    using lva::tr::EndpointActivity;
    using lva::tr::HomeButtonPress;
    using lva::tr::PhysicalControlPolicy;

    assert(PhysicalControlPolicy::OnHomeButton(
               HomeButtonPress::Single, EndpointActivity::Armed) ==
           ControlAction::ManualWake);
    assert(PhysicalControlPolicy::OnHomeButton(
               HomeButtonPress::Single, EndpointActivity::ActiveTurn) ==
           ControlAction::CancelTurn);
    assert(PhysicalControlPolicy::OnHomeButton(
               HomeButtonPress::Single, EndpointActivity::Playback) ==
           ControlAction::CancelTurn);
    assert(PhysicalControlPolicy::OnHomeButton(
               HomeButtonPress::Double, EndpointActivity::Armed) ==
           ControlAction::None);

    const auto active_mute = PhysicalControlPolicy::OnMuteChanged(
        true, true, EndpointActivity::ActiveTurn);
    assert(active_mute.notify_server);
    assert(active_mute.cancel_turn);
    const auto unmute = PhysicalControlPolicy::OnMuteChanged(
        false, true, EndpointActivity::ActiveTurn);
    assert(unmute.notify_server);
    assert(!unmute.cancel_turn);
}

void TestMuteCallbackSourcesAndHardwareSync() {
    char directory_template[] = "/tmp/cortana-controls-test-XXXXXX";
    const char* directory = ::mkdtemp(directory_template);
    assert(directory != nullptr);
    const std::filesystem::path gpio_path =
        std::filesystem::path(directory) / "gpio-value";
    {
        std::ofstream gpio(gpio_path);
        gpio << "0\n";
    }

    std::vector<std::pair<bool, lva::tr::MuteChangeSource>> changes;
    lva::tr::MicMuteGpio mute(
        [&changes](bool muted, lva::tr::MuteChangeSource source) {
            changes.emplace_back(muted, source);
        },
        gpio_path.string());
    assert(mute.Available());
    assert(mute.ReadAndApplyOnce());
    assert(changes.size() == 1);
    assert(changes.back().first);
    assert(changes.back().second == lva::tr::MuteChangeSource::InitialRead);

    {
        std::ofstream gpio(gpio_path, std::ios::trunc);
        gpio << "1\n";
    }
    mute.Poll();
    assert(changes.size() == 2);
    assert(!changes.back().first);
    assert(changes.back().second == lva::tr::MuteChangeSource::Hardware);

    mute.SyncToHardware(true);
    assert(changes.size() == 2);
    int value = -1;
    {
        std::ifstream gpio(gpio_path);
        gpio >> value;
    }
    assert(value == 0);
    std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
    TestLedPriorityAndExpiry();
    TestEndpointLedStateMapping();
    TestHomeButtonClassification();
    TestPhysicalControlPolicy();
    TestMuteCallbackSourcesAndHardwareSync();
}
