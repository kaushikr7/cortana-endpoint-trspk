#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace lva::audio {

struct PcmFormat {
    std::string encoding = "pcm_s16le";
    int sample_rate = 0;
    int sample_width = 0;
    int channels = 0;

    bool operator==(const PcmFormat&) const = default;
};

class PcmSink {
public:
    virtual ~PcmSink() = default;

    virtual bool Open(const PcmFormat& format, std::string& error) = 0;
    virtual bool Write(std::span<const std::byte> data,
                       std::string& error) = 0;
    virtual bool Drain(const std::function<bool()>& cancelled,
                       bool& was_cancelled,
                       std::string& error) = 0;
    virtual bool Flush(std::string& error) = 0;
    virtual void Close() noexcept = 0;
};

std::unique_ptr<PcmSink> MakePulseAudioSink(
    std::string sink_name,
    std::string stream_name = "Cortana response");

}  // namespace lva::audio
