#include "audio/PlaybackDmaKeepalive.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "util/Log.h"

namespace lva::audio {

namespace {

constexpr const char* kTag = "playback_keepalive";

}  // namespace

PlaybackDmaKeepalive::PlaybackDmaKeepalive(SinkFactory sink_factory)
    : PlaybackDmaKeepalive(Options{}, std::move(sink_factory)) {}

PlaybackDmaKeepalive::PlaybackDmaKeepalive(
    Options options, SinkFactory sink_factory)
    : options_(std::move(options)), sink_factory_(std::move(sink_factory)) {
    const auto& format = options_.format;
    if (!sink_factory_ || format.encoding != "pcm_s16le" ||
        format.sample_rate <= 0 || format.sample_width != 2 ||
        format.channels <= 0 ||
        options_.chunk_duration <= std::chrono::milliseconds::zero() ||
        options_.retry_delay <= std::chrono::milliseconds::zero() ||
        options_.startup_timeout <= std::chrono::milliseconds::zero() ||
        options_.startup_settle < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("invalid playback DMA keepalive options");
    }
}

PlaybackDmaKeepalive::~PlaybackDmaKeepalive() {
    Stop();
}

bool PlaybackDmaKeepalive::Start() {
    {
        std::lock_guard lock(mutex_);
        if (snapshot_.running) return snapshot_.ready;
        stopping_ = false;
        snapshot_.running = true;
        worker_ = std::thread([this] { Run(); });
    }

    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, options_.startup_timeout, [this] {
        return snapshot_.ready || !snapshot_.running;
    });
    const bool ready = snapshot_.ready;
    lock.unlock();
    if (ready && options_.startup_settle > std::chrono::milliseconds::zero()) {
        std::this_thread::sleep_for(options_.startup_settle);
    }
    return ready;
}

void PlaybackDmaKeepalive::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (!snapshot_.running && !worker_.joinable()) return;
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    snapshot_.running = false;
    snapshot_.ready = false;
}

PlaybackDmaKeepaliveSnapshot PlaybackDmaKeepalive::Snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void PlaybackDmaKeepalive::RecordFailure(std::string error) {
    std::lock_guard lock(mutex_);
    snapshot_.ready = false;
    ++snapshot_.errors;
    snapshot_.last_error = std::move(error);
    condition_.notify_all();
}

void PlaybackDmaKeepalive::Run() {
    const std::size_t bytes_per_second =
        static_cast<std::size_t>(options_.format.sample_rate) *
        static_cast<std::size_t>(options_.format.sample_width) *
        static_cast<std::size_t>(options_.format.channels);
    const std::size_t chunk_bytes = std::max<std::size_t>(
        static_cast<std::size_t>(options_.format.sample_width *
                                 options_.format.channels),
        bytes_per_second *
            static_cast<std::size_t>(options_.chunk_duration.count()) / 1000U);
    const std::vector<std::byte> silence(chunk_bytes, std::byte{0});
    bool opened_before = false;

    while (true) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) break;
        }

        std::unique_ptr<PcmSink> sink = sink_factory_();
        std::string error;
        if (!sink || !sink->Open(options_.format, error)) {
            if (error.empty()) error = "keepalive sink factory returned no sink";
            RecordFailure(error);
            LVA_LOGE(kTag, "open failed: %s", error.c_str());
        } else {
            {
                std::lock_guard lock(mutex_);
                ++snapshot_.streams_opened;
                if (opened_before) ++snapshot_.restarts;
            }
            opened_before = true;
            bool stream_failed = false;
            while (true) {
                {
                    std::lock_guard lock(mutex_);
                    if (stopping_) break;
                }
                if (!sink->Write(silence, error)) {
                    stream_failed = true;
                    RecordFailure(error);
                    LVA_LOGE(kTag, "write failed: %s", error.c_str());
                    break;
                }
                {
                    std::lock_guard lock(mutex_);
                    ++snapshot_.chunks_written;
                    snapshot_.ready = true;
                    snapshot_.last_error.clear();
                }
                condition_.notify_all();
            }
            sink->Close();
            if (!stream_failed) break;
        }

        std::unique_lock lock(mutex_);
        condition_.wait_for(lock, options_.retry_delay,
                            [this] { return stopping_; });
        if (stopping_) break;
    }

    std::lock_guard lock(mutex_);
    snapshot_.running = false;
    snapshot_.ready = false;
    condition_.notify_all();
}

}  // namespace lva::audio
