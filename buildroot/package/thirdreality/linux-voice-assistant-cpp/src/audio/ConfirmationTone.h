#pragma once

#include <string>

namespace lva::audio {

// Returns a short, gently faded 48 kHz stereo S16LE confirmation tone.
std::string MakeConfirmationTone();

}  // namespace lva::audio
