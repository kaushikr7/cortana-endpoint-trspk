#include "audio/PcmSink.h"

#include <pulse/error.h>
#include <pulse/simple.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>
#include <utility>

namespace lva::audio {

namespace {

class PulseAudioSink final : public PcmSink {
public:
    PulseAudioSink(std::string sink_name, std::string stream_name)
        : sink_name_(std::move(sink_name)),
          stream_name_(std::move(stream_name)) {}

    ~PulseAudioSink() override { Close(); }

    bool Open(const PcmFormat& format, std::string& error) override {
        Close();
        const pa_sample_spec sample_spec{
            .format = PA_SAMPLE_S16LE,
            .rate = static_cast<std::uint32_t>(format.sample_rate),
            .channels = static_cast<std::uint8_t>(format.channels),
        };

        // Bound PulseAudio's own device queue to roughly 100 ms. The player
        // maintains a separate bounded application queue.
        const std::uint64_t bytes_per_second =
            static_cast<std::uint64_t>(format.sample_rate) *
            static_cast<std::uint64_t>(format.sample_width) *
            static_cast<std::uint64_t>(format.channels);
        const pa_buffer_attr buffer_attr{
            .maxlength = std::numeric_limits<std::uint32_t>::max(),
            .tlength = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                bytes_per_second / 10,
                std::numeric_limits<std::uint32_t>::max())),
            .prebuf = 0,
            .minreq = std::numeric_limits<std::uint32_t>::max(),
            .fragsize = std::numeric_limits<std::uint32_t>::max(),
        };

        int pulse_error = 0;
        stream_ = pa_simple_new(
            nullptr,
            "cortana-endpoint-trspk",
            PA_STREAM_PLAYBACK,
            sink_name_.empty() ? nullptr : sink_name_.c_str(),
            stream_name_.c_str(),
            &sample_spec,
            nullptr,
            &buffer_attr,
            &pulse_error);
        if (stream_ != nullptr) return true;
        error = pa_strerror(pulse_error);
        return false;
    }

    bool Write(std::span<const std::byte> data,
               std::string& error) override {
        if (stream_ == nullptr) {
            error = "PulseAudio stream is not open";
            return false;
        }
        int pulse_error = 0;
        if (pa_simple_write(stream_, data.data(), data.size(),
                            &pulse_error) >= 0) {
            return true;
        }
        error = pa_strerror(pulse_error);
        return false;
    }

    bool Drain(const std::function<bool()>& cancelled,
               bool& was_cancelled,
               std::string& error) override {
        using namespace std::chrono_literals;
        was_cancelled = false;
        if (stream_ == nullptr) return true;
        // pa_simple_drain is blocking. Poll the server-side latency first so
        // a stop request can pre-empt the drain and flush promptly.
        while (true) {
            if (cancelled()) {
                was_cancelled = true;
                return true;
            }
            int latency_error = 0;
            const pa_usec_t latency =
                pa_simple_get_latency(stream_, &latency_error);
            if (latency == static_cast<pa_usec_t>(-1)) {
                error = pa_strerror(latency_error);
                return false;
            }
            if (latency <= 1000) break;
            std::this_thread::sleep_for(std::min(
                std::chrono::microseconds(5000),
                std::chrono::microseconds(latency)));
        }
        if (cancelled()) {
            was_cancelled = true;
            return true;
        }
        int pulse_error = 0;
        if (pa_simple_drain(stream_, &pulse_error) >= 0) return true;
        error = pa_strerror(pulse_error);
        return false;
    }

    bool Flush(std::string& error) override {
        if (stream_ == nullptr) return true;
        int pulse_error = 0;
        if (pa_simple_flush(stream_, &pulse_error) >= 0) return true;
        error = pa_strerror(pulse_error);
        return false;
    }

    void Close() noexcept override {
        if (stream_ == nullptr) return;
        pa_simple_free(stream_);
        stream_ = nullptr;
    }

private:
    std::string sink_name_;
    std::string stream_name_;
    pa_simple* stream_ = nullptr;
};

}  // namespace

std::unique_ptr<PcmSink> MakePulseAudioSink(
    std::string sink_name, std::string stream_name) {
    return std::make_unique<PulseAudioSink>(
        std::move(sink_name), std::move(stream_name));
}

}  // namespace lva::audio
