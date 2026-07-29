
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lva::audio {

class PcmRingBuffer {
   public:
    struct Metrics {
        std::size_t capacity_samples = 0;
        std::size_t queued_samples = 0;
        std::size_t high_watermark_samples = 0;
        std::uint64_t samples_written = 0;
        std::uint64_t samples_read = 0;
        std::uint64_t samples_dropped = 0;
        std::uint64_t samples_discarded = 0;
    };

    explicit PcmRingBuffer(std::size_t capacity_samples);

    PcmRingBuffer(const PcmRingBuffer&)            = delete;
    PcmRingBuffer& operator=(const PcmRingBuffer&) = delete;

    std::size_t Capacity() const noexcept { return mask_ + 1; }

    std::size_t Size() const noexcept;

    std::size_t FreeSpace() const noexcept;

    std::size_t Write(const std::int16_t* src, std::size_t n);

    std::size_t Read(std::int16_t* dst, std::size_t n);

    // Consumer-side, lock-free discard. Safe while the producer is writing.
    std::size_t Discard() noexcept;

    Metrics GetMetrics() const noexcept;

    // Reset is only safe before the producer starts or after it stops.
    void Reset() noexcept;

   private:
    std::vector<std::int16_t> buf_;
    std::size_t               mask_;
    std::atomic<std::size_t>  head_{0};
    std::atomic<std::size_t>  tail_{0};
    std::atomic<std::size_t>  high_watermark_{0};
    std::atomic<std::uint64_t> samples_written_{0};
    std::atomic<std::uint64_t> samples_read_{0};
    std::atomic<std::uint64_t> samples_dropped_{0};
    std::atomic<std::uint64_t> samples_discarded_{0};
};

}  // namespace lva::audio
