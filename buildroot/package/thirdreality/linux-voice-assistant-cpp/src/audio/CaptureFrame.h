#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lva::audio {

struct CaptureChannelLayout {
    unsigned channels = 4;
    unsigned microphone = 0;
    std::array<int, 2> reference = {2, 3};
};

bool ValidateCaptureLayout(const CaptureChannelLayout& layout) noexcept;

// Splits interleaved S16 input into a mono microphone period and, when
// configured, a mono AEC reference period. Two reference channels are averaged
// with 32-bit intermediate precision.
bool SplitCapturePeriod(std::span<const std::int16_t> interleaved,
                        std::size_t frames,
                        const CaptureChannelLayout& layout,
                        std::span<std::int16_t> microphone,
                        std::span<std::int16_t> reference) noexcept;

}  // namespace lva::audio
