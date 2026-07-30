#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "audio/RawPcmPlayer.h"

namespace {

using namespace std::chrono_literals;

struct SinkState {
    std::mutex mutex;
    std::condition_variable condition;
    bool opened = false;
    bool write_entered = false;
    bool block_write = false;
    bool release_write = false;
    bool fail_write = false;
    bool drain_entered = false;
    bool block_drain = false;
    bool release_drain = false;
    int flushes = 0;
    int closes = 0;
    lva::audio::PcmFormat format;
    std::vector<std::byte> written;
};

class FakeSink final : public lva::audio::PcmSink {
public:
    explicit FakeSink(std::shared_ptr<SinkState> state)
        : state_(std::move(state)) {}

    bool Open(const lva::audio::PcmFormat& format,
              std::string&) override {
        std::lock_guard lock(state_->mutex);
        state_->opened = true;
        state_->format = format;
        state_->condition.notify_all();
        return true;
    }

    bool Write(std::span<const std::byte> data,
               std::string& error) override {
        std::unique_lock lock(state_->mutex);
        state_->write_entered = true;
        state_->condition.notify_all();
        if (state_->block_write) {
            state_->condition.wait(lock, [this] {
                return state_->release_write;
            });
        }
        if (state_->fail_write) {
            error = "scripted write failure";
            return false;
        }
        state_->written.insert(state_->written.end(), data.begin(), data.end());
        return true;
    }

    bool Drain(const std::function<bool()>& cancelled,
               bool& was_cancelled,
               std::string&) override {
        std::unique_lock lock(state_->mutex);
        state_->drain_entered = true;
        was_cancelled = false;
        state_->condition.notify_all();
        if (state_->block_drain) {
            while (!state_->release_drain) {
                state_->condition.wait_for(lock, 1ms);
                if (cancelled()) {
                    was_cancelled = true;
                    return true;
                }
            }
        }
        return true;
    }

    bool Flush(std::string&) override {
        std::lock_guard lock(state_->mutex);
        ++state_->flushes;
        state_->condition.notify_all();
        return true;
    }

    void Close() noexcept override {
        std::lock_guard lock(state_->mutex);
        ++state_->closes;
        state_->condition.notify_all();
    }

private:
    std::shared_ptr<SinkState> state_;
};

bool WaitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

lva::audio::PcmFormat Format() {
    return {
        .encoding = "pcm_s16le",
        .sample_rate = 24000,
        .sample_width = 2,
        .channels = 1,
    };
}

std::string Samples(std::size_t bytes, char value) {
    return std::string(bytes, value);
}

void TestCompletionFollowsPhysicalDrain() {
    auto state = std::make_shared<SinkState>();
    state->block_drain = true;
    lva::audio::RawPcmPlayer player(
        {}, [state] { return std::make_unique<FakeSink>(state); });

    assert(player.Begin("turn-1", Format()));
    assert(player.Enqueue("turn-1", Samples(8, '\x11')));
    assert(player.End("turn-1"));
    assert(WaitUntil([&] {
        std::lock_guard lock(state->mutex);
        return state->drain_entered;
    }));
    const auto started = player.TryPopResult();
    assert(started.has_value());
    assert(started->outcome == lva::audio::RawPlaybackOutcome::Started);
    assert(!player.TryPopResult().has_value());
    {
        std::lock_guard lock(state->mutex);
        state->release_drain = true;
        state->condition.notify_all();
    }
    assert(WaitUntil([&] { return player.Snapshot().completed == 1; }));
    const auto result = player.TryPopResult();
    assert(result.has_value());
    assert(result->turn_id == "turn-1");
    assert(result->outcome == lva::audio::RawPlaybackOutcome::Completed);
    std::lock_guard lock(state->mutex);
    assert(state->format == Format());
    assert(state->written.size() == 8);
}

void TestBoundedQueueAndImmediateStop() {
    auto state = std::make_shared<SinkState>();
    state->block_write = true;
    lva::audio::RawPcmPlayer::Options options;
    options.maximum_queued_bytes = 8;
    options.maximum_chunk_bytes = 4;
    lva::audio::RawPcmPlayer player(
        options, [state] { return std::make_unique<FakeSink>(state); });

    assert(player.Begin("turn-2", Format()));
    assert(player.Enqueue("turn-2", Samples(4, '\x22')));
    assert(WaitUntil([&] {
        std::lock_guard lock(state->mutex);
        return state->write_entered;
    }));
    assert(player.Enqueue("turn-2", Samples(4, '\x33')));
    assert(!player.Enqueue("turn-2", Samples(4, '\x44')));
    player.Stop("turn-2", "cancelled");
    assert(player.Snapshot().queued_bytes == 0);
    assert(player.Snapshot().bytes_discarded == 8);
    {
        std::lock_guard lock(state->mutex);
        state->release_write = true;
        state->condition.notify_all();
    }
    assert(WaitUntil([&] { return player.Snapshot().stopped == 1; }));
    const auto result = player.TryPopResult();
    assert(result.has_value());
    assert(result->outcome == lva::audio::RawPlaybackOutcome::Stopped);
    std::lock_guard lock(state->mutex);
    assert(state->flushes == 1);
    assert(state->written.size() == 4);
}

void TestWriteFailureIsReportedWithoutBlockingProducer() {
    auto state = std::make_shared<SinkState>();
    state->fail_write = true;
    lva::audio::RawPcmPlayer player(
        {}, [state] { return std::make_unique<FakeSink>(state); });

    assert(player.Begin("turn-3", Format()));
    assert(player.Enqueue("turn-3", Samples(4, '\x66')));
    assert(WaitUntil([&] { return player.Snapshot().errors == 1; }));
    const auto result = player.TryPopResult();
    assert(result.has_value());
    assert(result->outcome == lva::audio::RawPlaybackOutcome::Error);
    assert(result->detail == "scripted write failure");
}

void TestStopInterruptsDrainWithoutCompleting() {
    auto state = std::make_shared<SinkState>();
    state->block_drain = true;
    lva::audio::RawPcmPlayer player(
        {}, [state] { return std::make_unique<FakeSink>(state); });

    assert(player.Begin("turn-drain", Format()));
    assert(player.Enqueue("turn-drain", Samples(4, '\x70')));
    assert(player.End("turn-drain"));
    assert(WaitUntil([&] {
        std::lock_guard lock(state->mutex);
        return state->drain_entered;
    }));
    player.Stop("turn-drain", "cancelled_during_drain");
    assert(WaitUntil([&] { return player.Snapshot().stopped == 1; }));
    assert(player.Snapshot().completed == 0);
    const auto started = player.TryPopResult();
    assert(started.has_value());
    assert(started->outcome == lva::audio::RawPlaybackOutcome::Started);
    const auto result = player.TryPopResult();
    assert(result.has_value());
    assert(result->outcome == lva::audio::RawPlaybackOutcome::Stopped);
}

void TestFormatAndTurnValidation() {
    auto state = std::make_shared<SinkState>();
    lva::audio::RawPcmPlayer player(
        {}, [state] { return std::make_unique<FakeSink>(state); });
    auto invalid = Format();
    invalid.encoding = "mp3";
    assert(!player.Begin("turn-4", invalid));
    assert(player.Begin("turn-4", Format()));
    assert(!player.Enqueue("wrong-turn", Samples(4, '\x77')));
    assert(!player.Enqueue("turn-4", Samples(3, '\x77')));
    player.Stop("turn-4", "test_done");
}

}  // namespace

int main() {
    TestCompletionFollowsPhysicalDrain();
    TestBoundedQueueAndImmediateStop();
    TestWriteFailureIsReportedWithoutBlockingProducer();
    TestStopInterruptsDrainWithoutCompleting();
    TestFormatAndTurnValidation();
}
