#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "audio/PcmSink.h"

namespace lva::audio {

struct PlaybackDmaKeepaliveSnapshot {
    bool running = false;
    bool ready = false;
    std::uint64_t streams_opened = 0;
    std::uint64_t chunks_written = 0;
    std::uint64_t restarts = 0;
    std::uint64_t errors = 0;
    std::string last_error;
};

class PlaybackDmaKeepalive {
public:
    using SinkFactory = std::function<std::unique_ptr<PcmSink>()>;

    struct Options {
        PcmFormat format{
            .encoding = "pcm_s16le",
            .sample_rate = 48000,
            .sample_width = 2,
            .channels = 2,
        };
        std::chrono::milliseconds chunk_duration{20};
        std::chrono::milliseconds retry_delay{1000};
        std::chrono::milliseconds startup_timeout{3000};
        std::chrono::milliseconds startup_settle{200};
    };

    explicit PlaybackDmaKeepalive(SinkFactory sink_factory);
    PlaybackDmaKeepalive(Options options, SinkFactory sink_factory);
    ~PlaybackDmaKeepalive();

    PlaybackDmaKeepalive(const PlaybackDmaKeepalive&) = delete;
    PlaybackDmaKeepalive& operator=(const PlaybackDmaKeepalive&) = delete;

    bool Start();
    void Stop();
    PlaybackDmaKeepaliveSnapshot Snapshot() const;

private:
    void Run();
    void RecordFailure(std::string error);

    Options options_;
    SinkFactory sink_factory_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool stopping_ = false;
    PlaybackDmaKeepaliveSnapshot snapshot_;
};

}  // namespace lva::audio
