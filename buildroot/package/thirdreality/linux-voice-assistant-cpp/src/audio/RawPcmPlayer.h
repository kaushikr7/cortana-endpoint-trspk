#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "audio/PcmSink.h"

namespace lva::audio {

enum class RawPlaybackState { Idle, Playing, Draining, Error };
enum class RawPlaybackOutcome { Started, Completed, Stopped, Error };

struct RawPlaybackResult {
    std::string turn_id;
    RawPlaybackOutcome outcome;
    std::string detail;
};

struct RawPlaybackSnapshot {
    RawPlaybackState state = RawPlaybackState::Idle;
    std::optional<std::string> turn_id;
    std::size_t queued_bytes = 0;
    std::size_t queue_high_watermark = 0;
    std::uint64_t bytes_accepted = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t bytes_rejected = 0;
    std::uint64_t bytes_discarded = 0;
    std::uint64_t started = 0;
    std::uint64_t completed = 0;
    std::uint64_t stopped = 0;
    std::uint64_t errors = 0;
    std::uint64_t dropped_results = 0;
};

class RawPcmPlayer {
public:
    struct Options {
        std::size_t maximum_queued_bytes = 256 * 1024;
        std::size_t maximum_chunk_bytes = 64 * 1024;
        std::size_t maximum_buffered_audio_ms = 500;
        std::size_t maximum_results = 8;
    };

    using SinkFactory = std::function<std::unique_ptr<PcmSink>()>;

    RawPcmPlayer(Options options, SinkFactory sink_factory);
    ~RawPcmPlayer();

    RawPcmPlayer(const RawPcmPlayer&) = delete;
    RawPcmPlayer& operator=(const RawPcmPlayer&) = delete;

    bool Begin(std::string turn_id, PcmFormat format);
    bool Enqueue(std::string_view turn_id, std::string payload);
    bool End(std::string_view turn_id);
    void Stop(std::string turn_id = {}, std::string reason = "stopped");

    std::optional<RawPlaybackResult> TryPopResult();
    RawPlaybackSnapshot Snapshot() const;

private:
    struct BeginCommand {
        std::string turn_id;
        PcmFormat format;
    };
    struct DataCommand {
        std::string turn_id;
        std::vector<std::byte> data;
    };
    struct EndCommand { std::string turn_id; };
    struct StopCommand {
        std::string turn_id;
        std::string reason;
    };
    using Command = std::variant<BeginCommand, DataCommand, EndCommand,
                                 StopCommand>;

    static bool ValidFormat(const PcmFormat& format);
    void Run();
    void PushResultLocked(RawPlaybackResult result);
    void SetStateLocked(RawPlaybackState state,
                        std::optional<std::string> turn_id = std::nullopt);

    Options options_;
    SinkFactory sink_factory_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Command> commands_;
    std::deque<RawPlaybackResult> results_;
    RawPlaybackSnapshot snapshot_;
    std::optional<std::string> accepting_turn_;
    std::size_t accepting_frame_bytes_ = 0;
    std::size_t accepting_queue_limit_bytes_ = 0;
    std::atomic<std::uint64_t> stop_epoch_{0};
    bool shutting_down_ = false;
    std::thread worker_;
};

}  // namespace lva::audio
