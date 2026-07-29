#include <array>
#include <cassert>
#include <cstdint>

#include "audio/CaptureFrame.h"
#include "audio/PcmRingBuffer.h"

namespace {

void TestChannelSplitAndReferenceMix() {
    const std::array<std::int16_t, 8> input = {
        100, 200, 300, 500,
        -100, -200, -301, -500,
    };
    std::array<std::int16_t, 2> microphone{};
    std::array<std::int16_t, 2> reference{};
    const lva::audio::CaptureChannelLayout layout{
        .channels = 4,
        .microphone = 0,
        .reference = {2, 3},
    };
    assert(lva::audio::SplitCapturePeriod(
        input, 2, layout, microphone, reference));
    assert((microphone == std::array<std::int16_t, 2>{100, -100}));
    assert((reference == std::array<std::int16_t, 2>{400, -400}));

    const lva::audio::CaptureChannelLayout no_reference{
        .channels = 4,
        .microphone = 1,
        .reference = {-1, -1},
    };
    assert(lva::audio::SplitCapturePeriod(
        input, 2, no_reference, microphone, {}));
    assert((microphone == std::array<std::int16_t, 2>{200, -200}));
}

void TestInvalidLayoutsFailClosed() {
    assert(!lva::audio::ValidateCaptureLayout({
        .channels = 4,
        .microphone = 4,
        .reference = {2, 3},
    }));
    assert(!lva::audio::ValidateCaptureLayout({
        .channels = 4,
        .microphone = 0,
        .reference = {-1, 2},
    }));
    assert(!lva::audio::ValidateCaptureLayout({
        .channels = 4,
        .microphone = 0,
        .reference = {2, 4},
    }));
    assert(!lva::audio::ValidateCaptureLayout({
        .channels = 4,
        .microphone = 0,
        .reference = {0, 3},
    }));
    assert(!lva::audio::ValidateCaptureLayout({
        .channels = 4,
        .microphone = 0,
        .reference = {2, 2},
    }));
}

void TestBoundedQueueMetricsAndDiscard() {
    lva::audio::PcmRingBuffer queue(5);
    assert(queue.Capacity() == 8);
    const std::array<std::int16_t, 6> first = {1, 2, 3, 4, 5, 6};
    assert(queue.Write(first.data(), first.size()) == first.size());

    std::array<std::int16_t, 3> read{};
    assert(queue.Read(read.data(), read.size()) == read.size());
    assert((read == std::array<std::int16_t, 3>{1, 2, 3}));

    const std::array<std::int16_t, 6> second = {7, 8, 9, 10, 11, 12};
    assert(queue.Write(second.data(), second.size()) == 5);
    assert(queue.Discard() == 8);

    const auto metrics = queue.GetMetrics();
    assert(metrics.capacity_samples == 8);
    assert(metrics.queued_samples == 0);
    assert(metrics.high_watermark_samples == 8);
    assert(metrics.samples_written == 11);
    assert(metrics.samples_read == 3);
    assert(metrics.samples_dropped == 1);
    assert(metrics.samples_discarded == 8);
}

}  // namespace

int main() {
    TestChannelSplitAndReferenceMix();
    TestInvalidLayoutsFailClosed();
    TestBoundedQueueMetricsAndDiscard();
}
