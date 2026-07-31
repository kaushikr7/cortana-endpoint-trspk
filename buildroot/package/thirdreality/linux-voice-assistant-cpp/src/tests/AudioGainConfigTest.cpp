#include <cassert>

#include "audio/CapturePipeline.h"
#include "audio/WebRtcProcessor.h"

int main() {
    const lva::audio::CapturePipeline::Options options;
    assert(options.automatic_gain_db == 42);
    assert(options.automatic_gain_db ==
           lva::audio::WebRtcProcessor::kDefaultGainDb);
    assert(lva::audio::WebRtcProcessor::kMaximumGainDb == 49);
}
