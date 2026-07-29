#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>

#include "config/EndpointConfig.h"
#include "cortana/DeviceTicket.h"

namespace lva::cortana {

bool LibcurlSupportsWebSockets() noexcept;

class DeviceTicketClient {
public:
    struct Options {
        std::filesystem::path ca_bundle =
            "/etc/ssl/certs/ca-certificates.crt";
        std::chrono::milliseconds connect_timeout{5000};
        std::chrono::milliseconds request_timeout{10000};
        std::size_t maximum_response_bytes = 64 * 1024;
    };

    using ClockFn = std::function<std::int64_t()>;

    DeviceTicketClient();
    explicit DeviceTicketClient(Options options);
    DeviceTicketClient(Options options, ClockFn clock);

    DeviceTicket Request(const lva::config::EndpointConfig& config) const;

private:
    Options options_;
    ClockFn clock_;
};

}  // namespace lva::cortana
