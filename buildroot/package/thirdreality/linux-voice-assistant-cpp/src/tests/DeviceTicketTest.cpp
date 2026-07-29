#include <cassert>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "cortana/DeviceTicket.h"
#include "cortana/Protocol.h"

namespace {

using Json = nlohmann::json;
using lva::cortana::TicketError;
using lva::cortana::TicketErrorCode;

Json ValidResponse(std::int64_t expires_at = 1'800'000'030) {
    return {
        {"ticket", std::string(64, 't')},
        {"expiresAt", expires_at},
        {"sessionPath", "/api/v1/voice/session"},
        {"protocolVersion", "1"},
        {"satellite",
         {
             {"satelliteId", "study-voice-1"},
             {"areaId", "study"},
             {"label", "Study voice endpoint"},
         }},
        {"capabilities",
         Json::parse(lva::cortana::SerializeCapabilitiesJson(
             lva::cortana::ContinuousDeviceCapabilities()))},
    };
}

lva::cortana::TicketExpectations Expectations() {
    return {
        .satellite_id = "study-voice-1",
        .expected_area_id = "study",
        .capabilities = lva::cortana::ContinuousDeviceCapabilities(),
    };
}

template <typename Function>
void ExpectError(TicketErrorCode code, Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const TicketError& error) {
        rejected = true;
        assert(error.code() == code);
    }
    assert(rejected);
}

void TestValidResponse() {
    const auto ticket = lva::cortana::ParseDeviceTicketResponse(
        ValidResponse().dump(), Expectations(), 1'800'000'000);
    assert(ticket.satellite.satellite_id == "study-voice-1");
    assert(ticket.satellite.area_id == "study");
    assert(ticket.expires_at == 1'800'000'030);
    assert(ticket.capabilities ==
           lva::cortana::ContinuousDeviceCapabilities());
}

void TestIdentityAndCapabilityFailures() {
    Json wrong_satellite = ValidResponse();
    wrong_satellite["satellite"]["satelliteId"] = "other-device";
    ExpectError(TicketErrorCode::IdentityMismatch, [&] {
        (void)lva::cortana::ParseDeviceTicketResponse(
            wrong_satellite.dump(), Expectations(), 1'800'000'000);
    });

    Json wrong_area = ValidResponse();
    wrong_area["satellite"]["areaId"] = "bedroom";
    ExpectError(TicketErrorCode::IdentityMismatch, [&] {
        (void)lva::cortana::ParseDeviceTicketResponse(
            wrong_area.dump(), Expectations(), 1'800'000'000);
    });

    Json wrong_capabilities = ValidResponse();
    wrong_capabilities["capabilities"]["bargeInMode"] = "full_duplex";
    ExpectError(TicketErrorCode::CapabilityMismatch, [&] {
        (void)lva::cortana::ParseDeviceTicketResponse(
            wrong_capabilities.dump(), Expectations(), 1'800'000'000);
    });
}

void TestExpiredMalformedAndExtraFields() {
    ExpectError(TicketErrorCode::Expired, [] {
        (void)lva::cortana::ParseDeviceTicketResponse(
            ValidResponse(1'800'000'000).dump(), Expectations(), 1'800'000'000);
    });
    Json extra = ValidResponse();
    extra["credential"] = "must-never-be-accepted";
    ExpectError(TicketErrorCode::InvalidResponse, [&] {
        (void)lva::cortana::ParseDeviceTicketResponse(
            extra.dump(), Expectations(), 1'800'000'000);
    });
    ExpectError(TicketErrorCode::InvalidResponse, [] {
        (void)lva::cortana::ParseDeviceTicketResponse(
            "not-json", Expectations(), 1'800'000'000);
    });
}

void TestClockReadiness() {
    assert(!lva::cortana::IsPlausibleSystemTime(0));
    assert(!lva::cortana::IsPlausibleSystemTime(1'704'067'199));
    assert(lva::cortana::IsPlausibleSystemTime(1'704'067'200));
}

}  // namespace

int main() {
    TestValidResponse();
    TestIdentityAndCapabilityFailures();
    TestExpiredMalformedAndExtraFields();
    TestClockReadiness();
}
