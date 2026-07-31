#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "audio/PcmRingBuffer.h"

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
        // Capture through PulseAudio's stable default source, which is backed
        // by the board's direct PDM device hw:0,2. The hw:0,4 loopback DAI
        // intermittently stops delivering periods on this hardware.
        std::string alsa_device = "cortana_capture";
        unsigned alsa_channels = 2;
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
    WebRtcProcessor* processor_ = nullptr;
    mutable std::mutex alsa_mutex_;
    void* alsa_handle_ = nullptr;
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
