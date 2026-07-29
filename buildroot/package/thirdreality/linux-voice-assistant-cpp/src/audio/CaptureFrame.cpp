#include "audio/CaptureFrame.h"

namespace lva::audio {

bool ValidateCaptureLayout(const CaptureChannelLayout& layout) noexcept {
    if (layout.channels == 0 || layout.microphone >= layout.channels) {
        return false;
    }
    const int first = layout.reference[0];
    const int second = layout.reference[1];
    if (first < -1 || second < -1) return false;
    if (first == -1) return second == -1;
    if (static_cast<unsigned>(first) >= layout.channels) return false;
    if (static_cast<unsigned>(first) == layout.microphone) return false;
    if (second >= 0 && static_cast<unsigned>(second) >= layout.channels) {
        return false;
    }
    if (second >= 0 &&
        (static_cast<unsigned>(second) == layout.microphone ||
         second == first)) {
        return false;
    }
    return true;
}

bool SplitCapturePeriod(std::span<const std::int16_t> interleaved,
                        std::size_t frames,
                        const CaptureChannelLayout& layout,
                        std::span<std::int16_t> microphone,
                        std::span<std::int16_t> reference) noexcept {
    if (!ValidateCaptureLayout(layout) ||
        interleaved.size() != frames * layout.channels ||
        microphone.size() < frames) {
        return false;
    }
    const int first = layout.reference[0];
    const int second = layout.reference[1];
    const bool has_reference = first >= 0;
    if (has_reference && reference.size() < frames) return false;

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::int16_t* row =
            interleaved.data() + frame * layout.channels;
        microphone[frame] = row[layout.microphone];
        if (!has_reference) continue;
        if (second < 0) {
            reference[frame] = row[first];
        } else {
            const std::int32_t mixed =
                static_cast<std::int32_t>(row[first]) +
                static_cast<std::int32_t>(row[second]);
            reference[frame] = static_cast<std::int16_t>(mixed / 2);
        }
    }
    return true;
}

}  // namespace lva::audio
