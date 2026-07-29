#include "cortana/DeviceTicket.h"

#include <initializer_list>
#include <set>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace lva::cortana {

namespace {

using Json = nlohmann::json;
constexpr std::int64_t kMinimumPlausibleTime = 1'704'067'200;  // 2024-01-01

void RequireKeys(const Json& value,
                 std::initializer_list<std::string_view> required) {
    if (!value.is_object()) {
        throw TicketError(TicketErrorCode::InvalidResponse,
                          "ticket response must be a JSON object");
    }
    std::set<std::string> allowed;
    for (const auto key : required) {
        allowed.emplace(key);
        if (!value.contains(key)) {
            throw TicketError(TicketErrorCode::InvalidResponse,
                              "ticket response is missing field: " +
                                  std::string(key));
        }
    }
    for (const auto& [key, item] : value.items()) {
        (void)item;
        if (!allowed.contains(key)) {
            throw TicketError(TicketErrorCode::InvalidResponse,
                              "ticket response has unknown field: " + key);
        }
    }
}

std::string ReadString(const Json& value,
                       std::string_view field,
                       std::size_t minimum,
                       std::size_t maximum) {
    const auto it = value.find(field);
    if (it == value.end() || !it->is_string()) {
        throw TicketError(TicketErrorCode::InvalidResponse,
                          "ticket response field must be a string: " +
                              std::string(field));
    }
    std::string result = it->get<std::string>();
    if (result.size() < minimum || result.size() > maximum) {
        throw TicketError(TicketErrorCode::InvalidResponse,
                          "ticket response field has invalid length: " +
                              std::string(field));
    }
    return result;
}

}  // namespace

bool IsPlausibleSystemTime(std::int64_t unix_seconds) noexcept {
    return unix_seconds >= kMinimumPlausibleTime;
}

DeviceTicket ParseDeviceTicketResponse(
    std::string_view payload,
    const TicketExpectations& expected,
    std::int64_t now_unix_seconds) {
    Json value;
    try {
        value = Json::parse(payload);
    } catch (...) {
        throw TicketError(TicketErrorCode::InvalidResponse,
                          "ticket response contains invalid JSON");
    }
    RequireKeys(value,
                {"ticket", "expiresAt", "sessionPath", "protocolVersion",
                 "satellite", "capabilities"});
    if (!value.at("expiresAt").is_number_integer()) {
        throw TicketError(TicketErrorCode::InvalidResponse,
                          "ticket response expiresAt must be an integer");
    }

    const Json& satellite = value.at("satellite");
    RequireKeys(satellite, {"satelliteId", "areaId", "label"});

    std::int64_t expires_at = 0;
    try {
        expires_at = value.at("expiresAt").get<std::int64_t>();
    } catch (...) {
        throw TicketError(TicketErrorCode::InvalidResponse,
                          "ticket response expiresAt is out of range");
    }

    DeviceTicket result{
        .ticket = ReadString(value, "ticket", 32, 4096),
        .expires_at = expires_at,
        .session_path = ReadString(value, "sessionPath", 1, 200),
        .protocol_version = ReadString(value, "protocolVersion", 1, 8),
        .satellite = {
            .satellite_id = ReadString(satellite, "satelliteId", 1, 100),
            .area_id = ReadString(satellite, "areaId", 1, 100),
            .label = ReadString(satellite, "label", 1, 200),
        },
        .capabilities = {},
    };
    try {
        result.capabilities =
            ParseCapabilitiesJson(value.at("capabilities").dump());
    } catch (const ProtocolError&) {
        throw TicketError(TicketErrorCode::InvalidResponse,
                          "ticket response contains invalid capabilities");
    }

    if (result.protocol_version != kProtocolVersion) {
        throw TicketError(TicketErrorCode::CapabilityMismatch,
                          "ticket protocol version is unsupported");
    }
    if (result.session_path != "/api/v1/voice/session") {
        throw TicketError(TicketErrorCode::InvalidResponse,
                          "ticket session path is unsupported");
    }
    if (result.satellite.satellite_id != expected.satellite_id) {
        throw TicketError(TicketErrorCode::IdentityMismatch,
                          "ticket satellite identity does not match this device");
    }
    if (expected.expected_area_id.has_value() &&
        result.satellite.area_id != *expected.expected_area_id) {
        throw TicketError(TicketErrorCode::IdentityMismatch,
                          "ticket area identity does not match commissioning "
                          "expectation");
    }
    if (result.capabilities != expected.capabilities) {
        throw TicketError(TicketErrorCode::CapabilityMismatch,
                          "ticket capabilities are unsupported by this device");
    }
    if (result.expires_at <= now_unix_seconds) {
        throw TicketError(TicketErrorCode::Expired,
                          "ticket was already expired when received");
    }
    return result;
}

}  // namespace lva::cortana
