#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "config/EndpointConfig.h"
#include "cortana/DeviceTicket.h"

namespace lva::cortana {

class SessionTransportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class WebSocketMessageKind { Text, Binary, Closed };

struct WebSocketMessage {
    WebSocketMessageKind kind;
    std::string payload;
    int close_code = 0;
    std::string close_reason;
};

class SessionTransport {
public:
    virtual ~SessionTransport() = default;

    virtual void SendText(std::string_view payload) = 0;
    virtual std::optional<WebSocketMessage> Receive(
        std::chrono::milliseconds timeout) = 0;
    virtual void Close(int code, std::string_view reason) noexcept = 0;
};

class SessionDependencies {
public:
    virtual ~SessionDependencies() = default;

    // Called once for every connection attempt. Device tickets are never
    // reused after a failed or closed WebSocket.
    virtual DeviceTicket RequestTicket(
        const lva::config::EndpointConfig& config) = 0;
    virtual std::unique_ptr<SessionTransport> Connect(
        std::string_view websocket_url) = 0;
};

std::string BuildWebSocketUrl(std::string_view https_origin,
                              std::string_view session_path);

}  // namespace lva::cortana
