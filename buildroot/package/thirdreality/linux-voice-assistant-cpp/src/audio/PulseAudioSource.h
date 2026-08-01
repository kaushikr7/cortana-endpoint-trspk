#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace lva::audio {

class PulseAudioSource {
public:
    struct Options {
        std::string source_name = "alsa_input.hw_0_2";
        unsigned sample_rate = 16'000;
        unsigned channels = 2;
        std::size_t frames_per_fragment = 160;
    };

    explicit PulseAudioSource(Options options);
    ~PulseAudioSource();

    PulseAudioSource(const PulseAudioSource&) = delete;
    PulseAudioSource& operator=(const PulseAudioSource&) = delete;

    bool Open(std::string& error);
    bool Read(std::span<std::int16_t> samples,
              const std::atomic<bool>& stop_requested,
              std::string& error);
    void Wake() noexcept;
    void Close() noexcept;

private:
    struct Impl;

    Options options_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lva::audio
