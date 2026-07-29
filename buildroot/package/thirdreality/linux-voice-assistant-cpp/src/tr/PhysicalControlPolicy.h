#pragma once

#include "tr/HomeButton.h"

namespace lva::tr {

enum class EndpointActivity { Unavailable, Armed, ActiveTurn, Playback };
enum class ControlAction { None, ManualWake, CancelTurn };

struct MuteDecision {
    bool notify_server = false;
    bool cancel_turn = false;
};

class PhysicalControlPolicy {
public:
    static ControlAction OnHomeButton(HomeButtonPress press,
                                      EndpointActivity activity) {
        if (press != HomeButtonPress::Single) return ControlAction::None;
        if (activity == EndpointActivity::Armed) {
            return ControlAction::ManualWake;
        }
        if (activity == EndpointActivity::ActiveTurn ||
            activity == EndpointActivity::Playback) {
            return ControlAction::CancelTurn;
        }
        return ControlAction::None;
    }

    static MuteDecision OnMuteChanged(bool muted,
                                      bool state_changed,
                                      EndpointActivity activity) {
        if (!state_changed) return {};
        return {
            .notify_server = true,
            .cancel_turn = muted &&
                (activity == EndpointActivity::ActiveTurn ||
                 activity == EndpointActivity::Playback),
        };
    }
};

}  // namespace lva::tr
