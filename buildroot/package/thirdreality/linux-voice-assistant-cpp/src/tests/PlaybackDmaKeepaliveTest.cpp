#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <thread>

#include "audio/PlaybackDmaKeepalive.h"

namespace {

using namespace std::chrono_literals;

struct FakeState {
    std::atomic<int> opens{0};
    std::atomic<int> writes{0};
    std::atomic<bool> fail_next_write{false};
};

class FakeSink final : public lva::audio::PcmSink {
public:
    explicit FakeSink(std::shared_ptr<FakeState> state)
        : state_(std::move(state)) {}

    bool Open(const lva::audio::PcmFormat& format,
              std::string&) override {
        assert(format.sample_rate == 48000);
        assert(format.channels == 2);
        ++state_->opens;
        return true;
    }

    bool Write(std::span<const std::byte> data,
               std::string& error) override {
        assert(data.size() == 3840);
        assert(std::all_of(data.begin(), data.end(),
                           [](std::byte value) {
                               return value == std::byte{0};
                           }));
        ++state_->writes;
        std::this_thread::sleep_for(1ms);
        if (state_->fail_next_write.exchange(false)) {
            error = "scripted keepalive write failure";
            return false;
        }
        return true;
    }

    bool Drain(const std::function<bool()>&, bool&,
               std::string&) override { return true; }
    bool Flush(std::string&) override { return true; }
    void Close() noexcept override {}

private:
    std::shared_ptr<FakeState> state_;
};

lva::audio::PlaybackDmaKeepalive::Options FastOptions() {
    lva::audio::PlaybackDmaKeepalive::Options options;
    options.retry_delay = 1ms;
    options.startup_timeout = 200ms;
    options.startup_settle = 0ms;
    return options;
}

void TestStartsWritesSilenceAndStops() {
    auto state = std::make_shared<FakeState>();
    lva::audio::PlaybackDmaKeepalive keepalive(
        FastOptions(),
        [state] { return std::make_unique<FakeSink>(state); });
    assert(keepalive.Start());
    const auto snapshot = keepalive.Snapshot();
    assert(snapshot.running);
    assert(snapshot.ready);
    assert(snapshot.streams_opened == 1);
    assert(snapshot.chunks_written >= 1);
    keepalive.Stop();
    assert(!keepalive.Snapshot().running);
}

void TestRecoversAFailedPulseStream() {
    auto state = std::make_shared<FakeState>();
    state->fail_next_write = true;
    lva::audio::PlaybackDmaKeepalive keepalive(
        FastOptions(),
        [state] { return std::make_unique<FakeSink>(state); });
    assert(keepalive.Start());
    const auto snapshot = keepalive.Snapshot();
    assert(snapshot.ready);
    assert(snapshot.streams_opened == 2);
    assert(snapshot.restarts == 1);
    assert(snapshot.errors == 1);
    assert(snapshot.chunks_written >= 1);
    keepalive.Stop();
}

}  // namespace

int main() {
    TestStartsWritesSilenceAndStops();
    TestRecoversAFailedPulseStream();
}
