#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include "audio/PcmRingBuffer.h"
#include "audio/PulseAudioSource.h"

namespace lva::audio {

class WebRtcProcessor;

struct AudioCaptureMetrics {
    bool running = false;
    bool aec_reference_enabled = false;
    std::size_t period_samples = 0;
    std::uint64_t periods_captured = 0;
    std::uint64_t samples_captured = 0;
    std::uint64_t reference_periods = 0;
    std::uint64_t recoveries = 0;
    std::uint64_t short_reads = 0;
    std::uint64_t processing_failures = 0;
    std::uint64_t last_period_monotonic_ns = 0;
    std::uint64_t maximum_processing_us = 0;
    PcmRingBuffer::Metrics queue;
};

class AudioCapture {
public:
    struct Options {
        // Capture through PulseAudio's native API from the direct PDM source.
        // The ALSA PulseAudio compatibility plugin intermittently blocks in
        // snd_pcm_readi even while this source remains healthy.
        std::string pulse_source = "alsa_input.hw_0_2";
        unsigned channels = 2;
        unsigned mic_channel = 0;
        std::array<int, 2> ref_channels = {-1, -1};
        std::size_t frames_per_read = 160;  // exactly 10 ms at 16 kHz
    };

    AudioCapture(Options options, PcmRingBuffer& queue);
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    void SetProcessor(WebRtcProcessor* processor);
    bool Start();
    void Stop();

    bool IsRunning() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }
    AudioCaptureMetrics GetMetrics() const noexcept;

private:
    void ThreadLoop();
    void RecordMaximumProcessing(
        std::chrono::steady_clock::duration duration) noexcept;

    Options options_;
    PcmRingBuffer& queue_;
    PulseAudioSource source_;
    WebRtcProcessor* processor_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint64_t> periods_captured_{0};
    std::atomic<std::uint64_t> samples_captured_{0};
    std::atomic<std::uint64_t> reference_periods_{0};
    std::atomic<std::uint64_t> recoveries_{0};
    std::atomic<std::uint64_t> short_reads_{0};
    std::atomic<std::uint64_t> processing_failures_{0};
    std::atomic<std::uint64_t> last_period_monotonic_ns_{0};
    std::atomic<std::uint64_t> maximum_processing_us_{0};
};

}  // namespace lva::audio
