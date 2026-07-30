#pragma once

#include "cortana/EndpointState.h"
#include "tr/LedController.h"

namespace lva::tr {

class EndpointLedPolicy {
public:
    static void Apply(const lva::cortana::EndpointSnapshot& endpoint,
                      LedController& leds);
};

}  // namespace lva::tr
