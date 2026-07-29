#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "audio/PcmRingBuffer.h"
#include "cortana/Protocol.h"
#include "cortana/SessionClient.h"

namespace lva::audio {

struct MicrophoneIngressMetrics {
    std::uint64_t generation = 0;
    std::uint64_t frames_assembled = 0;
    std::uint64_t frames_enqueued = 0;
    std::uint64_t frames_rejected = 0;
    std::uint64_t samples_discarded = 0;
};

class MicrophoneIngress {
public:
    using Frame = std::array<std::byte, lva::cortana::kMicrophoneFrameBytes>;
    using SendFrameFn = std::function<bool(std::uint64_t, const Frame&)>;

    MicrophoneIngress(PcmRingBuffer& queue,
                      SendFrameFn send_frame,
                      std::size_t maximum_frames_per_pump = 4);

    // Called by the endpoint main loop, never by the capture callback/thread.
    // Returns the number of frames accepted by the session queue.
    std::size_t Pump(const lva::cortana::SessionSnapshot& session,
                     bool muted);

    MicrophoneIngressMetrics GetMetrics() const noexcept { return metrics_; }

private:
    void DiscardQueue();
    static Frame PackFrame(
        const std::array<std::int16_t, 320>& samples) noexcept;

    PcmRingBuffer& queue_;
    SendFrameFn send_frame_;
    std::size_t maximum_frames_per_pump_;
    std::uint64_t active_generation_ = 0;
    MicrophoneIngressMetrics metrics_;
};

}  // namespace lva::audio
