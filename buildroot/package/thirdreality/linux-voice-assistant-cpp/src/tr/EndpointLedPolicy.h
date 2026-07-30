#pragma once

#include "audio/CaptureSupervisor.h"
#include "cortana/EndpointState.h"
#include "tr/LedController.h"

namespace lva::tr {

class EndpointLedPolicy {
public:
    static void Apply(const lva::cortana::EndpointSnapshot& endpoint,
                      LedController& leds);
    static void Apply(const lva::cortana::EndpointSnapshot& endpoint,
                      lva::audio::CaptureLifecycleState capture,
                      LedController& leds);
};

}  // namespace lva::tr
