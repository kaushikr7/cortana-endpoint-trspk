
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

namespace webrtc { class AudioProcessing; class StreamConfig; }

namespace lva::audio {

class WebRtcProcessor {
   public:
    static constexpr int kSampleRate     = 16'000;
    static constexpr int kFrameSamples   = 160;   // 10 ms @ 16 kHz
    static constexpr int kDefaultGainDb  = 42;
    static constexpr int kMaximumGainDb  = 49;

    WebRtcProcessor(int agc_level, int ns_level, bool aec_enabled);
    ~WebRtcProcessor();

    WebRtcProcessor(const WebRtcProcessor&)            = delete;
    WebRtcProcessor& operator=(const WebRtcProcessor&) = delete;

    void SetLevels(int agc_level, int ns_level);

    bool Process(std::int16_t* buf, std::size_t n);

    bool ProcessReverse(std::int16_t* buf, std::size_t n);

    // Rebuild all enabled processing after a capture discontinuity so AGC,
    // noise suppression, and optional AEC resume from fresh state.
    void Reset();

    int  agc_level()   const { return agc_level_; }
    int  ns_level()    const { return ns_level_; }
    bool aec_enabled() const { return aec_enabled_; }

   private:
    // Caller must hold apm_mutex_.
    void RebuildLocked();

    // Serializes all access to apm_: Process()/ProcessReverse()/Reset() run
    // on the capture thread, SetLevels() on the main
    // thread, so APM rebuilds must not race.
    std::mutex apm_mutex_;

    int  agc_level_   = 0;
    int  ns_level_    = 0;
    bool aec_enabled_ = false;

    void* apm_ = nullptr;
    std::unique_ptr<webrtc::StreamConfig> stream_cfg_;
};

}  // namespace lva::audio
