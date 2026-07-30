#pragma once

#include <cstddef>

#include "audio/AudioCapture.h"
#include "audio/PcmRingBuffer.h"
#include "audio/WebRtcProcessor.h"

namespace lva::audio {

class CapturePipeline {
public:
    struct Options {
        AudioCapture::Options capture;
        std::size_t queue_capacity_samples = 3200;  // rounded to 4096 / 256 ms
        int automatic_gain_db = 10;
        int noise_suppression_level = 2;
    };

    explicit CapturePipeline(Options options);

    CapturePipeline(const CapturePipeline&) = delete;
    CapturePipeline& operator=(const CapturePipeline&) = delete;

    bool Start();
    void Stop();
    std::size_t DiscardQueued() noexcept;
    void ResetProcessing();

    PcmRingBuffer& Queue() noexcept { return queue_; }
    const PcmRingBuffer& Queue() const noexcept { return queue_; }
    AudioCaptureMetrics GetMetrics() const noexcept {
        return capture_.GetMetrics();
    }

private:
    Options options_;
    PcmRingBuffer queue_;
    WebRtcProcessor processor_;
    AudioCapture capture_;
};

}  // namespace lva::audio
