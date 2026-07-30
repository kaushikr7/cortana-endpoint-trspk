#include "audio/CapturePipeline.h"

#include <stdexcept>
#include <utility>

namespace lva::audio {

namespace {

bool HasReference(const AudioCapture::Options& options) {
    return options.ref_channels[0] >= 0;
}

}  // namespace

CapturePipeline::CapturePipeline(Options options)
    : options_(std::move(options)),
      queue_(options_.queue_capacity_samples),
      processor_(options_.automatic_gain_db,
                 options_.noise_suppression_level,
                 HasReference(options_.capture)),
      capture_(options_.capture, queue_) {
    if (options_.queue_capacity_samples <
        options_.capture.frames_per_read * 2) {
        throw std::invalid_argument(
            "capture queue must hold at least two ALSA periods");
    }
    capture_.SetProcessor(&processor_);
}

bool CapturePipeline::Start() {
    return capture_.Start();
}

void CapturePipeline::Stop() {
    capture_.Stop();
}

std::size_t CapturePipeline::DiscardQueued() noexcept {
    return queue_.Discard();
}

void CapturePipeline::ResetProcessing() {
    processor_.ResetEcho();
}

}  // namespace lva::audio
