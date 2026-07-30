#include "tr/EndpointLedPolicy.h"

namespace lva::tr {

void EndpointLedPolicy::Apply(
    const lva::cortana::EndpointSnapshot& endpoint,
    LedController& leds) {
    Apply(endpoint, lva::audio::CaptureLifecycleState::Ready, leds);
}

void EndpointLedPolicy::Apply(
    const lva::cortana::EndpointSnapshot& endpoint,
    lva::audio::CaptureLifecycleState capture,
    LedController& leds) {
    using lva::cortana::Activity;
    using lva::cortana::SessionPhase;

    leds.SetBlocked(endpoint.phase == SessionPhase::Blocked);
    if (capture == lva::audio::CaptureLifecycleState::Blocked) {
        leds.SetSystem(LedState::Error);
    } else if (capture == lva::audio::CaptureLifecycleState::Degraded) {
        leds.SetSystem(LedState::Reconnecting);
    } else if (capture == lva::audio::CaptureLifecycleState::Starting) {
        leds.SetSystem(LedState::Booting);
    } else {
        leds.ClearSystem();
    }
    if (endpoint.phase == SessionPhase::Connecting ||
        endpoint.phase == SessionPhase::Negotiating) {
        leds.SetConnection(endpoint.generation <= 1
                               ? LedState::Booting
                               : LedState::Reconnecting);
        leds.ClearTurn();
        return;
    }
    if (endpoint.phase == SessionPhase::Backoff) {
        leds.SetConnection(LedState::Reconnecting);
        leds.ClearTurn();
        return;
    }
    if (endpoint.phase != SessionPhase::Ready) {
        leds.ClearConnection();
        leds.ClearTurn();
        return;
    }

    leds.ClearConnection();
    switch (endpoint.activity) {
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
            leds.SetTurn(LedState::Speaking);
            break;
        case Activity::Interrupting:
            leds.SetTurn(LedState::Cancelling);
            break;
        case Activity::Armed:
        case Activity::Idle:
            leds.ClearTurn();
            break;
    }
}

}  // namespace lva::tr
