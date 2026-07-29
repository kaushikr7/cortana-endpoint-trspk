#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>

#include "cortana/DeviceTicketClient.h"
#include "cortana/SessionTransport.h"

namespace lva::cortana {

struct CurlSessionOptions {
    std::filesystem::path ca_bundle =
        "/etc/ssl/certs/ca-certificates.crt";
    std::chrono::milliseconds connect_timeout{10000};
    std::size_t maximum_inbound_message_bytes = 1024 * 1024;
};

class CurlSessionDependencies final : public SessionDependencies {
public:
    CurlSessionDependencies();
    CurlSessionDependencies(DeviceTicketClient::Options ticket_options,
                            CurlSessionOptions websocket_options);

    DeviceTicket RequestTicket(
        const lva::config::EndpointConfig& config) override;
    std::unique_ptr<SessionTransport> Connect(
        std::string_view websocket_url) override;

private:
    DeviceTicketClient ticket_client_;
    CurlSessionOptions websocket_options_;
};

}  // namespace lva::cortana
