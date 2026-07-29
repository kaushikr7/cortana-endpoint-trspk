#include "audio/MicrophoneIngress.h"

#include <stdexcept>
#include <utility>

namespace lva::audio {

namespace {

constexpr std::size_t kSamplesPerPeriod = 160;
constexpr std::size_t kPeriodsPerFrame = 2;
constexpr std::size_t kSamplesPerFrame =
    kSamplesPerPeriod * kPeriodsPerFrame;

}  // namespace

MicrophoneIngress::MicrophoneIngress(PcmRingBuffer& queue,
                                     SendFrameFn send_frame,
                                     std::size_t maximum_frames_per_pump)
    : queue_(queue),
      send_frame_(std::move(send_frame)),
      maximum_frames_per_pump_(maximum_frames_per_pump) {
    if (!send_frame_ || maximum_frames_per_pump_ == 0) {
        throw std::invalid_argument("invalid microphone ingress options");
    }
}

std::size_t MicrophoneIngress::Pump(
    const lva::cortana::SessionSnapshot& session, bool muted) {
    if (session.phase != lva::cortana::SessionPhase::Ready ||
        !session.audio_started || muted || session.microphone_muted) {
        active_generation_ = 0;
        metrics_.generation = 0;
        DiscardQueue();
        return 0;
    }
    if (active_generation_ != session.generation) {
        active_generation_ = session.generation;
        metrics_.generation = session.generation;
        DiscardQueue();
        return 0;
    }

    std::size_t accepted = 0;
    for (std::size_t frame_index = 0;
         frame_index < maximum_frames_per_pump_;
         ++frame_index) {
        if (queue_.Size() < kSamplesPerFrame) break;
        std::array<std::int16_t, kSamplesPerFrame> samples{};
        if (queue_.Read(samples.data(), samples.size()) != samples.size()) {
            DiscardQueue();
            break;
        }
        Frame frame = PackFrame(samples);
        ++metrics_.frames_assembled;
        if (!send_frame_(active_generation_, frame)) {
            ++metrics_.frames_rejected;
            metrics_.samples_discarded += kSamplesPerFrame;
            DiscardQueue();
            return accepted;
        }
        ++metrics_.frames_enqueued;
        ++accepted;
    }

    // A delayed main loop must recover with fresh audio instead of carrying
    // old speech into the next transport opportunity.
    if (queue_.Size() >= kSamplesPerFrame) DiscardQueue();
    return accepted;
}

void MicrophoneIngress::DiscardQueue() {
    metrics_.samples_discarded += queue_.Discard();
}

MicrophoneIngress::Frame MicrophoneIngress::PackFrame(
    const std::array<std::int16_t, kSamplesPerFrame>& samples) noexcept {
    Frame frame{};
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const std::uint16_t value = static_cast<std::uint16_t>(samples[index]);
        frame[index * 2] = static_cast<std::byte>(value & 0xff);
        frame[index * 2 + 1] = static_cast<std::byte>((value >> 8) & 0xff);
    }
    return frame;
}

}  // namespace lva::audio
