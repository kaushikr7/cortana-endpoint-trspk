#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "cortana/Protocol.h"

namespace lva::cortana {

enum class TicketErrorCode {
    ClockNotReady,
    Network,
    Tls,
    AuthenticationRejected,
    ServiceUnavailable,
    Http,
    ResponseTooLarge,
    InvalidResponse,
    IdentityMismatch,
    CapabilityMismatch,
    Expired,
    Internal,
};

class TicketError : public std::runtime_error {
public:
    TicketError(TicketErrorCode code,
                std::string message,
                long http_status = 0)
        : std::runtime_error(std::move(message)),
          code_(code),
          http_status_(http_status) {}

    TicketErrorCode code() const noexcept { return code_; }
    long http_status() const noexcept { return http_status_; }

private:
    TicketErrorCode code_;
    long http_status_;
};

struct DeviceTicket {
    std::string ticket;
    std::int64_t expires_at;
    std::string session_path;
    std::string protocol_version;
    SatelliteIdentity satellite;
    Capabilities capabilities;
};

struct TicketExpectations {
    std::string satellite_id;
    std::optional<std::string> expected_area_id;
    Capabilities capabilities = ContinuousDeviceCapabilities();
};

bool IsPlausibleSystemTime(std::int64_t unix_seconds) noexcept;

DeviceTicket ParseDeviceTicketResponse(
    std::string_view payload,
    const TicketExpectations& expected,
    std::int64_t now_unix_seconds);

}  // namespace lva::cortana
