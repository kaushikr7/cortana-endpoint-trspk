#include "cortana/CurlSessionTransport.h"

#include <curl/curl.h>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace lva::cortana {

namespace {

using SteadyClock = std::chrono::steady_clock;

template <typename Value>
void SetOption(CURL* handle, CURLoption option, Value value) {
    if (::curl_easy_setopt(handle, option, value) != CURLE_OK) {
        throw SessionTransportError(
            "failed to configure Cortana WebSocket");
    }
}

void EnsureCurlInitialized() {
    static std::once_flag once;
    static CURLcode result = CURLE_FAILED_INIT;
    std::call_once(once, [] { result = ::curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (result != CURLE_OK) {
        throw SessionTransportError("failed to initialize WebSocket client");
    }
}

class CurlWebSocket final : public SessionTransport {
public:
    CurlWebSocket(std::string_view url, CurlSessionOptions options)
        : options_(std::move(options)),
          handle_(::curl_easy_init(), &::curl_easy_cleanup) {
        if (options_.connect_timeout <= std::chrono::milliseconds::zero() ||
            options_.maximum_inbound_message_bytes == 0) {
            throw SessionTransportError("invalid WebSocket transport options");
        }
        if (!LibcurlSupportsWebSockets()) {
            throw SessionTransportError(
                "target libcurl does not expose ws and wss protocols");
        }
        if (::access(options_.ca_bundle.c_str(), R_OK) != 0) {
            throw SessionTransportError(
                "system CA bundle is unavailable for WebSocket TLS");
        }
        if (!handle_) {
            throw SessionTransportError("failed to create Cortana WebSocket");
        }

        url_ = std::string(url);
        SetOption(handle_.get(), CURLOPT_URL, url_.c_str());
        SetOption(handle_.get(), CURLOPT_PROTOCOLS_STR, "wss");
        SetOption(handle_.get(), CURLOPT_REDIR_PROTOCOLS_STR, "wss");
        SetOption(handle_.get(), CURLOPT_FOLLOWLOCATION, 0L);
        SetOption(handle_.get(), CURLOPT_CONNECT_ONLY, 2L);
        SetOption(handle_.get(), CURLOPT_CONNECTTIMEOUT_MS,
                  static_cast<long>(options_.connect_timeout.count()));
        SetOption(handle_.get(), CURLOPT_TIMEOUT_MS,
                  static_cast<long>(options_.connect_timeout.count()));
        SetOption(handle_.get(), CURLOPT_NOSIGNAL, 1L);
        SetOption(handle_.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        SetOption(handle_.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        SetOption(handle_.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        SetOption(handle_.get(), CURLOPT_CAINFO,
                  options_.ca_bundle.c_str());
        SetOption(handle_.get(), CURLOPT_USERAGENT,
                  "cortana-endpoint-trspk/1");

        const CURLcode result = ::curl_easy_perform(handle_.get());
        if (result != CURLE_OK) {
            throw SessionTransportError(
                "Cortana WSS connection failed TLS or network validation");
        }
        if (::curl_easy_getinfo(handle_.get(), CURLINFO_ACTIVESOCKET,
                                &socket_) != CURLE_OK ||
            socket_ == CURL_SOCKET_BAD) {
            throw SessionTransportError(
                "Cortana WebSocket has no active socket");
        }
    }

    ~CurlWebSocket() override = default;

    void SendText(std::string_view payload) override {
        SendFrame(payload, CURLWS_TEXT);
    }

    std::optional<WebSocketMessage> Receive(
        std::chrono::milliseconds timeout) override {
        const auto deadline = SteadyClock::now() + timeout;
        while (true) {
            std::array<char, 16 * 1024> buffer{};
            std::size_t received = 0;
            const curl_ws_frame* metadata = nullptr;
            const CURLcode result = ::curl_ws_recv(
                handle_.get(), buffer.data(), buffer.size(), &received,
                &metadata);
            if (result == CURLE_AGAIN) {
                if (!Wait(POLLIN, deadline)) return std::nullopt;
                continue;
            }
            if (result == CURLE_GOT_NOTHING) {
                return WebSocketMessage{
                    .kind = WebSocketMessageKind::Closed,
                    .payload = {},
                    .close_code = 1006,
                    .close_reason = "connection lost",
                };
            }
            if (result != CURLE_OK || metadata == nullptr) {
                throw SessionTransportError(
                    "failed to receive Cortana WebSocket frame");
            }

            const std::string_view chunk(buffer.data(), received);
            if ((metadata->flags & CURLWS_PING) != 0) {
                // libcurl's frame mode has already sent the matching PONG.
                continue;
            }
            if ((metadata->flags & CURLWS_CLOSE) != 0) {
                int code = 1005;
                std::string reason;
                if (chunk.size() >= 2) {
                    code = (static_cast<unsigned char>(chunk[0]) << 8) |
                        static_cast<unsigned char>(chunk[1]);
                    reason.assign(chunk.substr(2));
                }
                closed_ = true;
                return WebSocketMessage{
                    .kind = WebSocketMessageKind::Closed,
                    .payload = {},
                    .close_code = code,
                    .close_reason = std::move(reason),
                };
            }

            const bool is_text = (metadata->flags & CURLWS_TEXT) != 0;
            const bool is_binary = (metadata->flags & CURLWS_BINARY) != 0;
            if (!is_text && !is_binary && inbound_.empty()) {
                continue;
            }
            if (inbound_.empty()) {
                inbound_kind_ = is_binary ? WebSocketMessageKind::Binary
                                          : WebSocketMessageKind::Text;
            } else if ((is_text && inbound_kind_ != WebSocketMessageKind::Text) ||
                       (is_binary &&
                        inbound_kind_ != WebSocketMessageKind::Binary)) {
                throw SessionTransportError(
                    "Cortana WebSocket changed message type mid-frame");
            }
            if (received > options_.maximum_inbound_message_bytes -
                    inbound_.size()) {
                throw SessionTransportError(
                    "Cortana WebSocket message exceeded its size limit");
            }
            inbound_.append(chunk);

            const bool fragment_complete = metadata->bytesleft == 0;
            const bool message_continues =
                (metadata->flags & CURLWS_CONT) != 0;
            if (fragment_complete && !message_continues) {
                WebSocketMessage message{
                    .kind = inbound_kind_,
                    .payload = std::move(inbound_),
                    .close_code = 0,
                    .close_reason = {},
                };
                inbound_.clear();
                return message;
            }
        }
    }

    void Close(int code, std::string_view reason) noexcept override {
        if (closed_ || !handle_) return;
        try {
            if (reason.size() > 123) reason = reason.substr(0, 123);
            std::string payload;
            payload.reserve(2 + reason.size());
            payload.push_back(static_cast<char>((code >> 8) & 0xff));
            payload.push_back(static_cast<char>(code & 0xff));
            payload.append(reason);
            SendFrame(payload, CURLWS_CLOSE);
        } catch (...) {
        }
        closed_ = true;
    }

private:
    bool Wait(short events, SteadyClock::time_point deadline) {
        while (true) {
            const auto now = SteadyClock::now();
            if (now >= deadline) return false;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now);
            pollfd descriptor{
                .fd = static_cast<int>(socket_),
                .events = events,
                .revents = 0,
            };
            const int result = ::poll(
                &descriptor, 1,
                static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
            if (result > 0) return true;
            if (result == 0) return false;
            if (errno != EINTR) {
                throw SessionTransportError(
                    "failed while waiting for Cortana WebSocket");
            }
        }
    }

    void SendFrame(std::string_view payload, unsigned int frame_type) {
        std::size_t offset = 0;
        bool first = true;
        while (true) {
            std::size_t sent = 0;
            const unsigned int flags = frame_type | CURLWS_OFFSET;
            const CURLcode result = ::curl_ws_send(
                handle_.get(), payload.data() + offset,
                payload.size() - offset, &sent,
                first ? static_cast<curl_off_t>(payload.size()) : 0,
                flags);
            if (result != CURLE_OK && result != CURLE_AGAIN) {
                throw SessionTransportError(
                    "failed to send Cortana WebSocket frame");
            }

            // libcurl may consume payload into its WebSocket send buffer and
            // still return CURLE_AGAIN while flushing the encoded bytes.
            // Advance by `sent` before polling so a retry cannot duplicate
            // part of the frame.
            offset += sent;
            if (sent > 0 || result == CURLE_OK) first = false;
            if (result == CURLE_OK && offset >= payload.size()) return;

            if (result == CURLE_AGAIN || sent == 0) {
                if (!Wait(POLLOUT,
                          SteadyClock::now() + options_.connect_timeout)) {
                    throw SessionTransportError(
                        "timed out sending Cortana WebSocket frame");
                }
            }
        }
    }

    CurlSessionOptions options_;
    std::string url_;
    std::unique_ptr<CURL, decltype(&::curl_easy_cleanup)> handle_;
    curl_socket_t socket_ = CURL_SOCKET_BAD;
    bool closed_ = false;
    std::string inbound_;
    WebSocketMessageKind inbound_kind_ = WebSocketMessageKind::Text;
};

}  // namespace

CurlSessionDependencies::CurlSessionDependencies()
    : CurlSessionDependencies(
          DeviceTicketClient::Options{}, CurlSessionOptions{}) {}

CurlSessionDependencies::CurlSessionDependencies(
    DeviceTicketClient::Options ticket_options,
    CurlSessionOptions websocket_options)
    : ticket_client_(ticket_options),
      websocket_options_(std::move(websocket_options)) {
    if (ticket_options.ca_bundle != websocket_options_.ca_bundle) {
        throw SessionTransportError(
            "ticket and WebSocket clients must use the same CA bundle");
    }
    EnsureCurlInitialized();
}

DeviceTicket CurlSessionDependencies::RequestTicket(
    const lva::config::EndpointConfig& config) {
    return ticket_client_.Request(config);
}

std::unique_ptr<SessionTransport> CurlSessionDependencies::Connect(
    std::string_view websocket_url) {
    return std::make_unique<CurlWebSocket>(
        websocket_url, websocket_options_);
}

}  // namespace lva::cortana
