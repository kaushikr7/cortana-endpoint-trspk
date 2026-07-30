#include "audio/RawPcmPlayer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "util/Log.h"

namespace lva::audio {

namespace {

constexpr const char* kTag = "pcm_player";

}  // namespace

RawPcmPlayer::RawPcmPlayer(Options options, SinkFactory sink_factory)
    : options_(std::move(options)), sink_factory_(std::move(sink_factory)) {
    if (!sink_factory_ || options_.maximum_queued_bytes == 0 ||
        options_.maximum_chunk_bytes == 0 ||
        options_.maximum_chunk_bytes > options_.maximum_queued_bytes ||
        options_.maximum_buffered_audio_ms == 0 ||
        options_.maximum_results == 0) {
        throw std::invalid_argument("invalid raw PCM player options");
    }
    worker_ = std::thread([this] { Run(); });
}

RawPcmPlayer::~RawPcmPlayer() {
    {
        std::lock_guard lock(mutex_);
        shutting_down_ = true;
        stop_epoch_.fetch_add(1, std::memory_order_relaxed);
        accepting_turn_.reset();
        accepting_frame_bytes_ = 0;
        accepting_queue_limit_bytes_ = 0;
        commands_.clear();
        snapshot_.bytes_discarded += snapshot_.queued_bytes;
        snapshot_.queued_bytes = 0;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool RawPcmPlayer::ValidFormat(const PcmFormat& format) {
    return format.encoding == "pcm_s16le" && format.sample_width == 2 &&
        format.sample_rate >= 8000 && format.sample_rate <= 192000 &&
        format.channels >= 1 && format.channels <= 2;
}

bool RawPcmPlayer::Begin(std::string turn_id, PcmFormat format) {
    std::lock_guard lock(mutex_);
    if (shutting_down_ || accepting_turn_.has_value() || turn_id.empty() ||
        !ValidFormat(format)) {
        return false;
    }
    accepting_turn_ = turn_id;
    accepting_frame_bytes_ = static_cast<std::size_t>(
        format.sample_width * format.channels);
    const std::size_t bytes_per_second =
        static_cast<std::size_t>(format.sample_rate) *
        accepting_frame_bytes_;
    accepting_queue_limit_bytes_ = std::min(
        options_.maximum_queued_bytes,
        bytes_per_second * options_.maximum_buffered_audio_ms / 1000);
    commands_.push_back(BeginCommand{std::move(turn_id), std::move(format)});
    condition_.notify_all();
    return true;
}

bool RawPcmPlayer::Enqueue(std::string_view turn_id, std::string payload) {
    std::lock_guard lock(mutex_);
    if (shutting_down_ || !accepting_turn_.has_value() ||
        *accepting_turn_ != turn_id || payload.empty() ||
        payload.size() > options_.maximum_chunk_bytes ||
        payload.size() % accepting_frame_bytes_ != 0 ||
        snapshot_.queued_bytes > accepting_queue_limit_bytes_ ||
        payload.size() > accepting_queue_limit_bytes_ -
                             snapshot_.queued_bytes) {
        snapshot_.bytes_rejected += payload.size();
        return false;
    }
    std::vector<std::byte> bytes(payload.size());
    std::memcpy(bytes.data(), payload.data(), payload.size());
    commands_.push_back(DataCommand{std::string(turn_id), std::move(bytes)});
    snapshot_.queued_bytes += payload.size();
    snapshot_.bytes_accepted += payload.size();
    snapshot_.queue_high_watermark = std::max(
        snapshot_.queue_high_watermark, snapshot_.queued_bytes);
    condition_.notify_all();
    return true;
}

bool RawPcmPlayer::End(std::string_view turn_id) {
    std::lock_guard lock(mutex_);
    if (shutting_down_ || !accepting_turn_.has_value() ||
        *accepting_turn_ != turn_id) {
        return false;
    }
    commands_.push_back(EndCommand{std::string(turn_id)});
    accepting_turn_.reset();
    accepting_frame_bytes_ = 0;
    accepting_queue_limit_bytes_ = 0;
    condition_.notify_all();
    return true;
}

void RawPcmPlayer::Stop(std::string turn_id, std::string reason) {
    std::lock_guard lock(mutex_);
    if (shutting_down_) return;
    if (turn_id.empty() && accepting_turn_.has_value()) {
        turn_id = *accepting_turn_;
    }
    accepting_turn_.reset();
    accepting_frame_bytes_ = 0;
    accepting_queue_limit_bytes_ = 0;
    stop_epoch_.fetch_add(1, std::memory_order_relaxed);
    commands_.clear();
    snapshot_.bytes_discarded += snapshot_.queued_bytes;
    snapshot_.queued_bytes = 0;
    commands_.push_front(StopCommand{
        .turn_id = std::move(turn_id),
        .reason = std::move(reason),
    });
    condition_.notify_all();
}

std::optional<RawPlaybackResult> RawPcmPlayer::TryPopResult() {
    std::lock_guard lock(mutex_);
    if (results_.empty()) return std::nullopt;
    RawPlaybackResult result = std::move(results_.front());
    results_.pop_front();
    return result;
}

RawPlaybackSnapshot RawPcmPlayer::Snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void RawPcmPlayer::SetStateLocked(
    RawPlaybackState state, std::optional<std::string> turn_id) {
    snapshot_.state = state;
    snapshot_.turn_id = std::move(turn_id);
}

void RawPcmPlayer::PushResultLocked(RawPlaybackResult result) {
    switch (result.outcome) {
        case RawPlaybackOutcome::Started: ++snapshot_.started; break;
        case RawPlaybackOutcome::Completed: ++snapshot_.completed; break;
        case RawPlaybackOutcome::Stopped: ++snapshot_.stopped; break;
        case RawPlaybackOutcome::Error: ++snapshot_.errors; break;
    }
    if (results_.size() >= options_.maximum_results) {
        results_.pop_front();
        ++snapshot_.dropped_results;
    }
    results_.push_back(std::move(result));
}

void RawPcmPlayer::Run() {
    std::unique_ptr<PcmSink> sink;
    std::string current_turn;
    std::size_t maximum_write_slice = 0;
    bool playback_started = false;

    while (true) {
        Command command;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] {
                return shutting_down_ || !commands_.empty();
            });
            if (shutting_down_) {
                lock.unlock();
                if (sink) {
                    std::string ignored;
                    (void)sink->Flush(ignored);
                    sink->Close();
                }
                return;
            }
            command = std::move(commands_.front());
            commands_.pop_front();
        }

        if (const auto* begin = std::get_if<BeginCommand>(&command)) {
            sink = sink_factory_();
            current_turn = begin->turn_id;
            playback_started = false;
            maximum_write_slice = static_cast<std::size_t>(
                begin->format.sample_rate * begin->format.sample_width *
                begin->format.channels / 50);  // 20 ms
            std::string error;
            if (!sink || !sink->Open(begin->format, error)) {
                if (error.empty()) error = "PCM sink factory returned no sink";
                if (sink) sink->Close();
                sink.reset();
                {
                    std::lock_guard lock(mutex_);
                    SetStateLocked(RawPlaybackState::Error);
                    PushResultLocked({current_turn,
                                      RawPlaybackOutcome::Error, error});
                }
                LVA_LOGE(kTag, "turn=%s open failed: %s",
                         current_turn.c_str(), error.c_str());
                current_turn.clear();
                continue;
            }
            {
                std::lock_guard lock(mutex_);
                SetStateLocked(RawPlaybackState::Playing, current_turn);
            }
            LVA_LOGI(kTag, "turn=%s started rate=%d channels=%d",
                     current_turn.c_str(), begin->format.sample_rate,
                     begin->format.channels);
            continue;
        }

        if (const auto* data = std::get_if<DataCommand>(&command)) {
            if (!sink || data->turn_id != current_turn) {
                std::lock_guard lock(mutex_);
                const std::size_t discarded = std::min(
                    data->data.size(), snapshot_.queued_bytes);
                snapshot_.queued_bytes -= discarded;
                snapshot_.bytes_discarded += discarded;
                continue;
            }
            const auto write_epoch =
                stop_epoch_.load(std::memory_order_relaxed);
            std::size_t offset = 0;
            std::string error;
            bool write_failed = false;
            bool cancelled = false;
            while (offset < data->data.size()) {
                if (stop_epoch_.load(std::memory_order_relaxed) !=
                    write_epoch) {
                    cancelled = true;
                    break;
                }
                const std::size_t bytes = std::min(
                    maximum_write_slice, data->data.size() - offset);
                if (!sink->Write(
                        std::span<const std::byte>(data->data).subspan(
                            offset, bytes),
                        error)) {
                    write_failed = true;
                    break;
                }
                {
                    std::lock_guard lock(mutex_);
                    if (stop_epoch_.load(std::memory_order_relaxed) !=
                        write_epoch) {
                        cancelled = true;
                    } else {
                        snapshot_.queued_bytes -= bytes;
                        snapshot_.bytes_written += bytes;
                        if (!playback_started) {
                            playback_started = true;
                            PushResultLocked({
                                current_turn,
                                RawPlaybackOutcome::Started,
                                {},
                            });
                        }
                        offset += bytes;
                    }
                }
                if (cancelled) break;
            }
            if (!write_failed && !cancelled) {
                continue;
            }
            std::string ignored;
            (void)sink->Flush(ignored);
            sink->Close();
            sink.reset();
            if (cancelled) {
                std::lock_guard lock(mutex_);
                SetStateLocked(RawPlaybackState::Idle);
                current_turn.clear();
                continue;
            }
            {
                std::lock_guard lock(mutex_);
                const std::size_t remaining = data->data.size() - offset;
                const std::size_t discarded = std::min(
                    remaining, snapshot_.queued_bytes);
                snapshot_.queued_bytes -= discarded;
                snapshot_.bytes_discarded += discarded;
                SetStateLocked(RawPlaybackState::Error);
                PushResultLocked({current_turn, RawPlaybackOutcome::Error,
                                  error});
            }
            LVA_LOGE(kTag, "turn=%s write failed: %s",
                     current_turn.c_str(), error.c_str());
            current_turn.clear();
            continue;
        }

        if (const auto* end = std::get_if<EndCommand>(&command)) {
            if (!sink || end->turn_id != current_turn) continue;
            {
                std::lock_guard lock(mutex_);
                SetStateLocked(RawPlaybackState::Draining, current_turn);
            }
            const auto drain_epoch =
                stop_epoch_.load(std::memory_order_relaxed);
            std::string error;
            bool cancelled = false;
            const bool drained = sink->Drain(
                [this, drain_epoch] {
                    return stop_epoch_.load(std::memory_order_relaxed) !=
                        drain_epoch;
                },
                cancelled,
                error);
            if (cancelled) {
                std::string ignored;
                (void)sink->Flush(ignored);
                sink->Close();
                sink.reset();
                {
                    std::lock_guard lock(mutex_);
                    SetStateLocked(RawPlaybackState::Idle);
                }
                current_turn.clear();
                continue;
            }
            sink->Close();
            sink.reset();
            {
                std::lock_guard lock(mutex_);
                SetStateLocked(drained ? RawPlaybackState::Idle
                                       : RawPlaybackState::Error);
                PushResultLocked({
                    current_turn,
                    drained ? RawPlaybackOutcome::Completed
                            : RawPlaybackOutcome::Error,
                    drained ? std::string{} : error,
                });
            }
            LVA_LOGI(kTag, "turn=%s %s", current_turn.c_str(),
                     drained ? "drained" : "drain failed");
            current_turn.clear();
            continue;
        }

        const auto& stop = std::get<StopCommand>(command);
        const std::string stopped_turn =
            !current_turn.empty() ? current_turn : stop.turn_id;
        std::string error;
        bool flushed = true;
        if (sink) {
            flushed = sink->Flush(error);
            sink->Close();
            sink.reset();
        }
        {
            std::lock_guard lock(mutex_);
            SetStateLocked(flushed ? RawPlaybackState::Idle
                                   : RawPlaybackState::Error);
            if (!stopped_turn.empty()) {
                PushResultLocked({
                    stopped_turn,
                    flushed ? RawPlaybackOutcome::Stopped
                            : RawPlaybackOutcome::Error,
                    flushed ? stop.reason : error,
                });
            }
        }
        current_turn.clear();
    }
}

}  // namespace lva::audio
