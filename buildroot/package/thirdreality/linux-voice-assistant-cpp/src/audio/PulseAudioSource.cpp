#include "audio/PulseAudioSource.h"

#include <pulse/error.h>
#include <pulse/pulseaudio.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace lva::audio {

struct PulseAudioSource::Impl {
    pa_threaded_mainloop* mainloop = nullptr;
    pa_context* context = nullptr;
    pa_stream* stream = nullptr;
    bool mainloop_started = false;
    std::vector<std::byte> pending;
    std::size_t pending_offset = 0;

    static void SignalContext(pa_context*, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        pa_threaded_mainloop_signal(self->mainloop, 0);
    }

    static void SignalStream(pa_stream*, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        pa_threaded_mainloop_signal(self->mainloop, 0);
    }

    static void SignalRead(pa_stream*, std::size_t, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        pa_threaded_mainloop_signal(self->mainloop, 0);
    }
};

namespace {

bool ContextReady(pa_context* context) {
    return pa_context_get_state(context) == PA_CONTEXT_READY;
}

bool StreamReady(pa_stream* stream) {
    return pa_stream_get_state(stream) == PA_STREAM_READY;
}

std::string PulseError(pa_context* context) {
    return pa_strerror(context == nullptr ? PA_ERR_UNKNOWN
                                         : pa_context_errno(context));
}

}  // namespace

PulseAudioSource::PulseAudioSource(Options options)
    : options_(std::move(options)) {}

PulseAudioSource::~PulseAudioSource() {
    Close();
}

bool PulseAudioSource::Open(std::string& error) {
    Close();
    if (options_.sample_rate == 0 || options_.channels == 0 ||
        options_.channels > std::numeric_limits<std::uint8_t>::max() ||
        options_.frames_per_fragment == 0) {
        error = "invalid PulseAudio capture format";
        return false;
    }
    impl_ = std::make_unique<Impl>();
    Impl& state = *impl_;
    state.mainloop = pa_threaded_mainloop_new();
    if (state.mainloop == nullptr) {
        error = "could not create PulseAudio main loop";
        Close();
        return false;
    }
    state.context = pa_context_new(
        pa_threaded_mainloop_get_api(state.mainloop),
        "cortana-endpoint-trspk-capture");
    if (state.context == nullptr) {
        error = "could not create PulseAudio context";
        Close();
        return false;
    }
    pa_context_set_state_callback(
        state.context, &Impl::SignalContext, &state);
    if (pa_threaded_mainloop_start(state.mainloop) < 0) {
        error = "could not start PulseAudio main loop";
        Close();
        return false;
    }
    state.mainloop_started = true;

    pa_threaded_mainloop_lock(state.mainloop);
    if (pa_context_connect(state.context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) <
        0) {
        error = PulseError(state.context);
        pa_threaded_mainloop_unlock(state.mainloop);
        Close();
        return false;
    }
    while (!ContextReady(state.context)) {
        if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(state.context))) {
            error = PulseError(state.context);
            pa_threaded_mainloop_unlock(state.mainloop);
            Close();
            return false;
        }
        pa_threaded_mainloop_wait(state.mainloop);
    }

    const pa_sample_spec sample_spec{
        .format = PA_SAMPLE_S16LE,
        .rate = options_.sample_rate,
        .channels = static_cast<std::uint8_t>(options_.channels),
    };
    state.stream = pa_stream_new(
        state.context, "Cortana microphone", &sample_spec, nullptr);
    if (state.stream == nullptr) {
        error = PulseError(state.context);
        pa_threaded_mainloop_unlock(state.mainloop);
        Close();
        return false;
    }
    pa_stream_set_state_callback(state.stream, &Impl::SignalStream, &state);
    pa_stream_set_read_callback(state.stream, &Impl::SignalRead, &state);

    const std::uint64_t fragment_bytes =
        options_.frames_per_fragment * options_.channels *
        sizeof(std::int16_t);
    const std::uint64_t maximum_bytes =
        static_cast<std::uint64_t>(options_.sample_rate) *
        options_.channels * sizeof(std::int16_t) / 2;
    if (fragment_bytes > std::numeric_limits<std::uint32_t>::max() ||
        maximum_bytes > std::numeric_limits<std::uint32_t>::max() ||
        fragment_bytes > maximum_bytes) {
        error = "PulseAudio capture fragment is too large";
        pa_threaded_mainloop_unlock(state.mainloop);
        Close();
        return false;
    }
    const pa_buffer_attr buffer_attr{
        .maxlength = static_cast<std::uint32_t>(maximum_bytes),
        .tlength = std::numeric_limits<std::uint32_t>::max(),
        .prebuf = std::numeric_limits<std::uint32_t>::max(),
        .minreq = std::numeric_limits<std::uint32_t>::max(),
        .fragsize = static_cast<std::uint32_t>(fragment_bytes),
    };
    if (pa_stream_connect_record(
            state.stream,
            options_.source_name.empty() ? nullptr
                                         : options_.source_name.c_str(),
            &buffer_attr,
            PA_STREAM_ADJUST_LATENCY) < 0) {
        error = PulseError(state.context);
        pa_threaded_mainloop_unlock(state.mainloop);
        Close();
        return false;
    }
    while (!StreamReady(state.stream)) {
        if (!PA_STREAM_IS_GOOD(pa_stream_get_state(state.stream)) ||
            !PA_CONTEXT_IS_GOOD(pa_context_get_state(state.context))) {
            error = PulseError(state.context);
            pa_threaded_mainloop_unlock(state.mainloop);
            Close();
            return false;
        }
        pa_threaded_mainloop_wait(state.mainloop);
    }
    pa_threaded_mainloop_unlock(state.mainloop);
    return true;
}

bool PulseAudioSource::Read(
    std::span<std::int16_t> samples,
    const std::atomic<bool>& stop_requested,
    std::string& error) {
    error.clear();
    if (impl_ == nullptr || impl_->stream == nullptr) {
        error = "PulseAudio capture stream is not open";
        return false;
    }
    Impl& state = *impl_;
    auto* output = reinterpret_cast<std::byte*>(samples.data());
    const std::size_t output_bytes = samples.size_bytes();
    std::size_t copied = 0;

    while (copied < output_bytes) {
        const std::size_t pending_bytes =
            state.pending.size() - state.pending_offset;
        if (pending_bytes != 0) {
            const std::size_t count =
                std::min(pending_bytes, output_bytes - copied);
            std::memcpy(output + copied,
                        state.pending.data() + state.pending_offset,
                        count);
            state.pending_offset += count;
            copied += count;
            if (state.pending_offset == state.pending.size()) {
                state.pending.clear();
                state.pending_offset = 0;
            }
            continue;
        }

        pa_threaded_mainloop_lock(state.mainloop);
        while (!stop_requested.load(std::memory_order_acquire)) {
            if (!ContextReady(state.context) || !StreamReady(state.stream)) {
                error = PulseError(state.context);
                pa_threaded_mainloop_unlock(state.mainloop);
                return false;
            }
            const std::size_t readable = pa_stream_readable_size(state.stream);
            if (readable == static_cast<std::size_t>(-1)) {
                error = PulseError(state.context);
                pa_threaded_mainloop_unlock(state.mainloop);
                return false;
            }
            if (readable != 0) break;
            pa_threaded_mainloop_wait(state.mainloop);
        }
        if (stop_requested.load(std::memory_order_acquire)) {
            pa_threaded_mainloop_unlock(state.mainloop);
            return false;
        }

        const void* data = nullptr;
        std::size_t bytes = 0;
        if (pa_stream_peek(state.stream, &data, &bytes) < 0) {
            error = PulseError(state.context);
            pa_threaded_mainloop_unlock(state.mainloop);
            return false;
        }
        if (bytes != 0) {
            const auto previous = state.pending.size();
            state.pending.resize(previous + bytes);
            if (data == nullptr) {
                std::fill(state.pending.begin() + previous,
                          state.pending.end(), std::byte{0});
            } else {
                std::memcpy(state.pending.data() + previous, data, bytes);
            }
        }
        if (pa_stream_drop(state.stream) < 0) {
            error = PulseError(state.context);
            pa_threaded_mainloop_unlock(state.mainloop);
            return false;
        }
        pa_threaded_mainloop_unlock(state.mainloop);
    }
    return true;
}

void PulseAudioSource::Wake() noexcept {
    if (impl_ == nullptr || impl_->mainloop == nullptr ||
        !impl_->mainloop_started) {
        return;
    }
    pa_threaded_mainloop_lock(impl_->mainloop);
    pa_threaded_mainloop_signal(impl_->mainloop, 0);
    pa_threaded_mainloop_unlock(impl_->mainloop);
}

void PulseAudioSource::Close() noexcept {
    if (impl_ == nullptr) return;
    Impl& state = *impl_;
    if (state.mainloop != nullptr && state.mainloop_started) {
        pa_threaded_mainloop_lock(state.mainloop);
    }
    if (state.stream != nullptr) {
        pa_stream_set_read_callback(state.stream, nullptr, nullptr);
        pa_stream_set_state_callback(state.stream, nullptr, nullptr);
        pa_stream_disconnect(state.stream);
        pa_stream_unref(state.stream);
        state.stream = nullptr;
    }
    if (state.context != nullptr) {
        pa_context_set_state_callback(state.context, nullptr, nullptr);
        pa_context_disconnect(state.context);
        pa_context_unref(state.context);
        state.context = nullptr;
    }
    if (state.mainloop != nullptr && state.mainloop_started) {
        pa_threaded_mainloop_unlock(state.mainloop);
        pa_threaded_mainloop_stop(state.mainloop);
        state.mainloop_started = false;
    }
    if (state.mainloop != nullptr) {
        pa_threaded_mainloop_free(state.mainloop);
        state.mainloop = nullptr;
    }
    impl_.reset();
}

}  // namespace lva::audio
