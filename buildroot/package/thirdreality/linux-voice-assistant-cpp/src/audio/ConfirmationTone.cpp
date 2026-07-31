#include "audio/ConfirmationTone.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lva::audio {

std::string MakeConfirmationTone() {
    constexpr int kSampleRate = 48000;
    constexpr int kChannels = 2;
    constexpr int kDurationMs = 80;
    constexpr int kFadeMs = 8;
    constexpr double kFrequencyHz = 880.0;
    constexpr double kAmplitude = 0.20 * 32767.0;
    constexpr int kFrames = kSampleRate * kDurationMs / 1000;
    constexpr int kFadeFrames = kSampleRate * kFadeMs / 1000;
    constexpr double kPi = 3.14159265358979323846;

    std::string pcm(static_cast<std::size_t>(kFrames * kChannels * 2), '\0');
    for (int frame = 0; frame < kFrames; ++frame) {
        const double attack = std::min(1.0,
            static_cast<double>(frame) / kFadeFrames);
        const double release = std::min(1.0,
            static_cast<double>(kFrames - 1 - frame) / kFadeFrames);
        const double envelope = std::max(0.0, std::min(attack, release));
        const double phase = 2.0 * kPi * kFrequencyHz *
            static_cast<double>(frame) / kSampleRate;
        const auto sample = static_cast<std::int16_t>(
            std::lround(std::sin(phase) * kAmplitude * envelope));
        const auto encoded = static_cast<std::uint16_t>(sample);
        for (int channel = 0; channel < kChannels; ++channel) {
            const std::size_t offset = static_cast<std::size_t>(
                (frame * kChannels + channel) * 2);
            pcm[offset] = static_cast<char>(encoded & 0xff);
            pcm[offset + 1] = static_cast<char>((encoded >> 8) & 0xff);
        }
    }
    return pcm;
}

}  // namespace lva::audio
