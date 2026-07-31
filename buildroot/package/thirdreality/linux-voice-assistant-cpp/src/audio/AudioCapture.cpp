#include "audio/AudioCapture.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
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
    : options_(std::move(options)), queue_(queue) {}

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
        .channels = options_.alsa_channels,
        .microphone = options_.mic_channel,
        .reference = options_.ref_channels,
    };
    if (!ValidateCaptureLayout(layout) ||
        options_.frames_per_read != kWebRtcPeriodSamples) {
        LVA_LOGE(kTag,
                 "invalid ALSA capture layout or period "
                 "(channels=%u mic=%u ref=%d,%d period=%zu)",
                 options_.alsa_channels, options_.mic_channel,
                 options_.ref_channels[0], options_.ref_channels[1],
                 options_.frames_per_read);
        return false;
    }

    snd_pcm_t* pcm = nullptr;
    int result = ::snd_pcm_open(&pcm, options_.alsa_device.c_str(),
                                SND_PCM_STREAM_CAPTURE, 0);
    if (result < 0) {
        LVA_LOGE(kTag, "snd_pcm_open(%s) failed: %s",
                 options_.alsa_device.c_str(), ::snd_strerror(result));
        return false;
    }

    snd_pcm_hw_params_t* hardware = nullptr;
    snd_pcm_hw_params_alloca(&hardware);
    auto fail = [&](const char* operation, int error) {
        LVA_LOGE(kTag, "%s failed: %s", operation, ::snd_strerror(error));
        ::snd_pcm_close(pcm);
        return false;
    };

    if ((result = ::snd_pcm_hw_params_any(pcm, hardware)) < 0) {
        return fail("snd_pcm_hw_params_any", result);
    }
    if ((result = ::snd_pcm_hw_params_set_access(
             pcm, hardware, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        return fail("snd_pcm_hw_params_set_access", result);
    }
    if ((result = ::snd_pcm_hw_params_set_format(
             pcm, hardware, SND_PCM_FORMAT_S16_LE)) < 0) {
        return fail("snd_pcm_hw_params_set_format", result);
    }
    if ((result = ::snd_pcm_hw_params_set_channels(
             pcm, hardware, options_.alsa_channels)) < 0) {
        return fail("snd_pcm_hw_params_set_channels", result);
    }
    unsigned rate = kSampleRate;
    if ((result = ::snd_pcm_hw_params_set_rate_near(
             pcm, hardware, &rate, nullptr)) < 0) {
        return fail("snd_pcm_hw_params_set_rate_near", result);
    }
    if (rate != kSampleRate) {
        LVA_LOGE(kTag, "ALSA selected rate %u instead of %u", rate,
                 kSampleRate);
        ::snd_pcm_close(pcm);
        return false;
    }

    snd_pcm_uframes_t period = options_.frames_per_read;
    if ((result = ::snd_pcm_hw_params_set_period_size_near(
             pcm, hardware, &period, nullptr)) < 0) {
        return fail("snd_pcm_hw_params_set_period_size_near", result);
    }
    snd_pcm_uframes_t buffer_frames = kSampleRate / 2;
    if ((result = ::snd_pcm_hw_params_set_buffer_size_near(
             pcm, hardware, &buffer_frames)) < 0) {
        return fail("snd_pcm_hw_params_set_buffer_size_near", result);
    }
    if ((result = ::snd_pcm_hw_params(pcm, hardware)) < 0) {
        return fail("snd_pcm_hw_params", result);
    }
    snd_pcm_uframes_t actual_buffer = 0;
    snd_pcm_uframes_t actual_period = 0;
    if ((result = ::snd_pcm_get_params(
             pcm, &actual_buffer, &actual_period)) < 0) {
        return fail("snd_pcm_get_params", result);
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
    {
        std::lock_guard lock(alsa_mutex_);
        alsa_handle_ = pcm;
    }
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { ThreadLoop(); });

    LVA_LOGI(kTag,
             "started ALSA device=%s rate=%u channels=%u read=%zu "
             "period=%lu buffer=%lu mic=%u reference=%d,%d queue=%zu",
             options_.alsa_device.c_str(), kSampleRate,
             options_.alsa_channels, options_.frames_per_read,
             static_cast<unsigned long>(actual_period),
             static_cast<unsigned long>(actual_buffer),
             options_.mic_channel, options_.ref_channels[0],
             options_.ref_channels[1], queue_.Capacity());
    return true;
}

void AudioCapture::Stop() {
    stop_requested_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(alsa_mutex_);
        if (alsa_handle_ != nullptr) {
            (void)::snd_pcm_abort(static_cast<snd_pcm_t*>(alsa_handle_));
        }
    }
    if (thread_.joinable()) thread_.join();
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
        .channels = options_.alsa_channels,
        .microphone = options_.mic_channel,
        .reference = options_.ref_channels,
    };
    const bool has_reference = options_.ref_channels[0] >= 0;
    std::vector<std::int16_t> interleaved(period * options_.alsa_channels);
    std::vector<std::int16_t> microphone(period);
    std::vector<std::int16_t> reference(period);
    snd_pcm_t* pcm = static_cast<snd_pcm_t*>(alsa_handle_);
    std::uint64_t last_logged_drops = 0;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        const snd_pcm_sframes_t read =
            ::snd_pcm_readi(pcm, interleaved.data(), period);
        if (read < 0) {
            const int error = static_cast<int>(read);
            if (error == -EPIPE || error == -ESTRPIPE || error == -EINTR ||
                error == -EIO) {
                if (::snd_pcm_recover(pcm, error, 1) < 0) break;
                recoveries_.fetch_add(1, std::memory_order_relaxed);
                if (processor_ != nullptr) processor_->Reset();
                continue;
            }
            if (!stop_requested_.load(std::memory_order_relaxed)) {
                LVA_LOGE(kTag, "snd_pcm_readi failed: %s",
                         ::snd_strerror(error));
            }
            break;
        }
        if (static_cast<std::size_t>(read) != period) {
            short_reads_.fetch_add(1, std::memory_order_relaxed);
            if (processor_ != nullptr) processor_->Reset();
            continue;
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

    {
        std::lock_guard lock(alsa_mutex_);
        alsa_handle_ = nullptr;
        ::snd_pcm_close(pcm);
    }
    running_.store(false, std::memory_order_release);
    LVA_LOGI(kTag, "capture stopped periods=%llu samples=%llu",
             static_cast<unsigned long long>(
                 periods_captured_.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(
                 samples_captured_.load(std::memory_order_relaxed)));
}

}  // namespace lva::audio
