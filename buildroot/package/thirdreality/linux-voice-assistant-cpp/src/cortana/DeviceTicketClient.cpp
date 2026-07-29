#include "cortana/DeviceTicketClient.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <unistd.h>

#include <nlohmann/json.hpp>

namespace lva::cortana {

namespace {

void Wipe(char* data, std::size_t size) noexcept {
    volatile char* cursor = data;
    while (size-- > 0) *cursor++ = '\0';
}

class SensitiveString {
public:
    explicit SensitiveString(std::string value) : value_(std::move(value)) {}
    ~SensitiveString() {
        Wipe(value_.data(), value_.size());
    }

    SensitiveString(const SensitiveString&) = delete;
    SensitiveString& operator=(const SensitiveString&) = delete;

    const char* c_str() const { return value_.c_str(); }

private:
    std::string value_;
};

void SecureFreeHeaders(struct curl_slist* headers) noexcept {
    for (struct curl_slist* header = headers;
         header != nullptr;
         header = header->next) {
        if (header->data != nullptr) {
            Wipe(header->data, std::strlen(header->data));
        }
    }
    ::curl_slist_free_all(headers);
}

struct SecureHeaderDeleter {
    void operator()(struct curl_slist* headers) const noexcept {
        SecureFreeHeaders(headers);
    }
};

struct ResponseBuffer {
    std::string value;
    std::size_t maximum_bytes;
    bool too_large = false;
};

std::size_t AppendResponse(char* data,
                           std::size_t size,
                           std::size_t count,
                           void* context) {
    auto& response = *static_cast<ResponseBuffer*>(context);
    if (size != 0 && count > static_cast<std::size_t>(-1) / size) {
        response.too_large = true;
        return 0;
    }
    const std::size_t bytes = size * count;
    if (bytes > response.maximum_bytes - response.value.size()) {
        response.too_large = true;
        return 0;
    }
    response.value.append(data, bytes);
    return bytes;
}

template <typename Value>
void SetOption(CURL* handle, CURLoption option, Value value) {
    if (::curl_easy_setopt(handle, option, value) != CURLE_OK) {
        throw TicketError(TicketErrorCode::Internal,
                          "failed to configure HTTPS ticket request");
    }
}

void EnsureCurlInitialized() {
    static std::once_flag once;
    static CURLcode result = CURLE_FAILED_INIT;
    std::call_once(once, [] { result = ::curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (result != CURLE_OK) {
        throw TicketError(TicketErrorCode::Internal,
                          "failed to initialize HTTPS client");
    }
}

bool IsTlsFailure(CURLcode code) {
    switch (code) {
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:
            return true;
        default:
            return false;
    }
}

void AppendHeader(struct curl_slist** headers, const char* value) {
    struct curl_slist* updated = ::curl_slist_append(*headers, value);
    if (updated == nullptr) {
        SecureFreeHeaders(*headers);
        *headers = nullptr;
        throw TicketError(TicketErrorCode::Internal,
                          "failed to create HTTPS request headers");
    }
    *headers = updated;
}

}  // namespace

bool LibcurlSupportsWebSockets() noexcept {
    const curl_version_info_data* info =
        ::curl_version_info(CURLVERSION_NOW);
    if (info == nullptr || info->protocols == nullptr) return false;
    bool websocket = false;
    bool secure_websocket = false;
    for (const char* const* protocol = info->protocols;
         *protocol != nullptr;
         ++protocol) {
        websocket |= std::string_view(*protocol) == "ws";
        secure_websocket |= std::string_view(*protocol) == "wss";
    }
    return websocket && secure_websocket;
}

DeviceTicketClient::DeviceTicketClient()
    : DeviceTicketClient(Options{}) {}

DeviceTicketClient::DeviceTicketClient(Options options)
    : DeviceTicketClient(
          std::move(options),
          [] { return static_cast<std::int64_t>(std::time(nullptr)); }) {}

DeviceTicketClient::DeviceTicketClient(Options options, ClockFn clock)
    : options_(std::move(options)), clock_(std::move(clock)) {
    if (!clock_) {
        throw TicketError(TicketErrorCode::Internal,
                          "ticket client clock is not configured");
    }
    EnsureCurlInitialized();
}

DeviceTicket DeviceTicketClient::Request(
    const lva::config::EndpointConfig& config) const {
    const std::int64_t now = clock_();
    if (!IsPlausibleSystemTime(now)) {
        throw TicketError(TicketErrorCode::ClockNotReady,
                          "system clock is not ready for TLS validation");
    }
    if (::access(options_.ca_bundle.c_str(), R_OK) != 0) {
        throw TicketError(TicketErrorCode::Tls,
                          "system CA bundle is unavailable");
    }

    EnsureCurlInitialized();
    std::unique_ptr<CURL, decltype(&::curl_easy_cleanup)> handle(
        ::curl_easy_init(), &::curl_easy_cleanup);
    if (!handle) {
        throw TicketError(TicketErrorCode::Internal,
                          "failed to create HTTPS ticket request");
    }

    const std::string url =
        config.endpoint + "/api/v1/voice/device-ticket";
    const std::string request_body = nlohmann::json(
        {{"satellite_id", config.satellite_id}}).dump();
    SensitiveString authorization("Authorization: Bearer " + config.credential);

    struct curl_slist* raw_headers = nullptr;
    AppendHeader(&raw_headers, "Content-Type: application/json");
    AppendHeader(&raw_headers, "Accept: application/json");
    AppendHeader(&raw_headers, authorization.c_str());
    std::unique_ptr<curl_slist, SecureHeaderDeleter> headers(raw_headers);
    if (!headers) {
        throw TicketError(TicketErrorCode::Internal,
                          "failed to create HTTPS request headers");
    }

    ResponseBuffer response{
        .value = {},
        .maximum_bytes = options_.maximum_response_bytes,
    };
    std::array<char, CURL_ERROR_SIZE> curl_error{};

    SetOption(handle.get(), CURLOPT_URL, url.c_str());
    SetOption(handle.get(), CURLOPT_PROTOCOLS_STR, "https");
    SetOption(handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    SetOption(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
    SetOption(handle.get(), CURLOPT_POST, 1L);
    SetOption(handle.get(), CURLOPT_POSTFIELDS, request_body.data());
    SetOption(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE,
              static_cast<curl_off_t>(request_body.size()));
    SetOption(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    SetOption(handle.get(), CURLOPT_CONNECTTIMEOUT_MS,
              static_cast<long>(options_.connect_timeout.count()));
    SetOption(handle.get(), CURLOPT_TIMEOUT_MS,
              static_cast<long>(options_.request_timeout.count()));
    SetOption(handle.get(), CURLOPT_NOSIGNAL, 1L);
    SetOption(handle.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    SetOption(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    SetOption(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    SetOption(handle.get(), CURLOPT_CAINFO, options_.ca_bundle.c_str());
    SetOption(handle.get(), CURLOPT_WRITEFUNCTION, &AppendResponse);
    SetOption(handle.get(), CURLOPT_WRITEDATA, &response);
    SetOption(handle.get(), CURLOPT_ERRORBUFFER, curl_error.data());
    SetOption(handle.get(), CURLOPT_USERAGENT, "cortana-endpoint-trspk/1");

    const CURLcode result = ::curl_easy_perform(handle.get());
    if (response.too_large) {
        throw TicketError(TicketErrorCode::ResponseTooLarge,
                          "ticket response exceeded its size limit");
    }
    if (result != CURLE_OK) {
        if (IsTlsFailure(result)) {
            throw TicketError(TicketErrorCode::Tls,
                              "TLS validation failed for ticket request");
        }
        throw TicketError(TicketErrorCode::Network,
                          "ticket request could not reach Cortana");
    }

    long status = 0;
    if (::curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) !=
        CURLE_OK) {
        throw TicketError(TicketErrorCode::Internal,
                          "ticket response status is unavailable");
    }
    if (status == 401 || status == 403) {
        throw TicketError(TicketErrorCode::AuthenticationRejected,
                          "device authentication was rejected", status);
    }
    if (status == 503) {
        throw TicketError(TicketErrorCode::ServiceUnavailable,
                          "device ticket service is unavailable", status);
    }
    if (status < 200 || status >= 300) {
        throw TicketError(TicketErrorCode::Http,
                          "device ticket request failed", status);
    }

    TicketExpectations expected{
        .satellite_id = config.satellite_id,
        .expected_area_id = config.expected_area_id.empty()
            ? std::nullopt
            : std::optional<std::string>(config.expected_area_id),
        .capabilities = ContinuousDeviceCapabilities(),
    };
    return ParseDeviceTicketResponse(response.value, expected, clock_());
}

}  // namespace lva::cortana
