#include "audio/AudioCapture.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "audio/CaptureFrame.h"
#include "audio/WebRtcProcessor.h"
#include "util/Log.h"

namespace lva::audio {

namespace {

constexpr const char* kTag = "capture";
constexpr unsigned kSampleRate = 16'000;
constexpr std::size_t kWebRtcPeriodSamples = 160;

std::uint64_t MonotonicNanoseconds() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

AudioCapture::AudioCapture(Options options, PcmRingBuffer& queue)
    : options_(std::move(options)),
      queue_(queue),
      source_({
          .source_name = options_.pulse_source,
          .sample_rate = kSampleRate,
          .channels = options_.channels,
          .frames_per_fragment = options_.frames_per_read,
      }) {}

AudioCapture::~AudioCapture() {
    Stop();
}

void AudioCapture::SetProcessor(WebRtcProcessor* processor) {
    if (running_.load(std::memory_order_relaxed)) {
        LVA_LOGW(kTag, "%s", "SetProcessor called after Start; ignoring");
        return;
    }
    processor_ = processor;
}

bool AudioCapture::Start() {
    if (running_.load(std::memory_order_relaxed)) return true;
    if (thread_.joinable()) thread_.join();

    const CaptureChannelLayout layout{
        .channels = options_.channels,
        .microphone = options_.mic_channel,
        .reference = options_.ref_channels,
    };
    if (!ValidateCaptureLayout(layout) ||
        options_.frames_per_read != kWebRtcPeriodSamples) {
        LVA_LOGE(kTag,
                 "invalid capture layout or period "
                 "(channels=%u mic=%u ref=%d,%d period=%zu)",
                 options_.channels, options_.mic_channel,
                 options_.ref_channels[0], options_.ref_channels[1],
                 options_.frames_per_read);
        return false;
    }

    std::string error;
    if (!source_.Open(error)) {
        LVA_LOGE(kTag, "PulseAudio source %s failed: %s",
                 options_.pulse_source.c_str(), error.c_str());
        return false;
    }
    queue_.Reset();
    periods_captured_.store(0, std::memory_order_relaxed);
    samples_captured_.store(0, std::memory_order_relaxed);
    reference_periods_.store(0, std::memory_order_relaxed);
    recoveries_.store(0, std::memory_order_relaxed);
    short_reads_.store(0, std::memory_order_relaxed);
    processing_failures_.store(0, std::memory_order_relaxed);
    last_period_monotonic_ns_.store(0, std::memory_order_relaxed);
    maximum_processing_us_.store(0, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { ThreadLoop(); });

    LVA_LOGI(kTag,
             "started PulseAudio source=%s rate=%u channels=%u "
             "fragment=%zu mic=%u reference=%d,%d queue=%zu",
             options_.pulse_source.c_str(), kSampleRate,
             options_.channels, options_.frames_per_read,
             options_.mic_channel, options_.ref_channels[0],
             options_.ref_channels[1], queue_.Capacity());
    return true;
}

void AudioCapture::Stop() {
    stop_requested_.store(true, std::memory_order_release);
    source_.Wake();
    if (thread_.joinable()) thread_.join();
    source_.Close();
    running_.store(false, std::memory_order_release);
}

AudioCaptureMetrics AudioCapture::GetMetrics() const noexcept {
    return {
        .running = running_.load(std::memory_order_relaxed),
        .aec_reference_enabled = options_.ref_channels[0] >= 0,
        .period_samples = options_.frames_per_read,
        .periods_captured = periods_captured_.load(std::memory_order_relaxed),
        .samples_captured = samples_captured_.load(std::memory_order_relaxed),
        .reference_periods =
            reference_periods_.load(std::memory_order_relaxed),
        .recoveries = recoveries_.load(std::memory_order_relaxed),
        .short_reads = short_reads_.load(std::memory_order_relaxed),
        .processing_failures =
            processing_failures_.load(std::memory_order_relaxed),
        .last_period_monotonic_ns =
            last_period_monotonic_ns_.load(std::memory_order_relaxed),
        .maximum_processing_us =
            maximum_processing_us_.load(std::memory_order_relaxed),
        .queue = queue_.GetMetrics(),
    };
}

void AudioCapture::RecordMaximumProcessing(
    std::chrono::steady_clock::duration duration) noexcept {
    const auto microseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
    std::uint64_t maximum =
        maximum_processing_us_.load(std::memory_order_relaxed);
    while (microseconds > maximum &&
           !maximum_processing_us_.compare_exchange_weak(
               maximum, microseconds, std::memory_order_relaxed)) {
    }
}

void AudioCapture::ThreadLoop() {
    const std::size_t period = options_.frames_per_read;
    const CaptureChannelLayout layout{
        .channels = options_.channels,
        .microphone = options_.mic_channel,
        .reference = options_.ref_channels,
    };
    const bool has_reference = options_.ref_channels[0] >= 0;
    std::vector<std::int16_t> interleaved(period * options_.channels);
    std::vector<std::int16_t> microphone(period);
    std::vector<std::int16_t> reference(period);
    std::uint64_t last_logged_drops = 0;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        std::string error;
        if (!source_.Read(interleaved, stop_requested_, error)) {
            if (!stop_requested_.load(std::memory_order_relaxed) &&
                !error.empty()) {
                LVA_LOGE(kTag, "PulseAudio capture failed: %s",
                         error.c_str());
            }
            break;
        }

        const auto processing_started = std::chrono::steady_clock::now();
        if (!SplitCapturePeriod(interleaved, period, layout, microphone,
                                reference)) {
            processing_failures_.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        bool processed = true;
        if (processor_ != nullptr) {
            if (has_reference) {
                processed = processor_->ProcessReverse(
                    reference.data(), period);
                if (processed) {
                    reference_periods_.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            processed = processed &&
                processor_->Process(microphone.data(), period);
        }
        if (!processed) {
            processing_failures_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        (void)queue_.Write(microphone.data(), period);
        periods_captured_.fetch_add(1, std::memory_order_relaxed);
        samples_captured_.fetch_add(period, std::memory_order_relaxed);
        last_period_monotonic_ns_.store(
            MonotonicNanoseconds(), std::memory_order_relaxed);
        RecordMaximumProcessing(
            std::chrono::steady_clock::now() - processing_started);

        const auto queue_metrics = queue_.GetMetrics();
        if (queue_metrics.samples_dropped - last_logged_drops >= 160'000) {
            last_logged_drops = queue_metrics.samples_dropped;
            LVA_LOGW(kTag, "bounded PCM queue dropped %llu samples",
                     static_cast<unsigned long long>(last_logged_drops));
        }
    }

    running_.store(false, std::memory_order_release);
    LVA_LOGI(kTag, "capture stopped periods=%llu samples=%llu",
             static_cast<unsigned long long>(
                 periods_captured_.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 samples_captured_.load(std::memory_order_relaxed)));
}

}  // namespace lva::audio
