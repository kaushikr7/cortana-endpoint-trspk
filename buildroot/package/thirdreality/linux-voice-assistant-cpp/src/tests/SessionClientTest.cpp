#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/EndpointConfig.h"
#include "cortana/Protocol.h"
#include "cortana/SessionClient.h"
#include "cortana/SessionTransport.h"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;
using lva::cortana::SessionPhase;
using lva::cortana::WebSocketMessage;
using lva::cortana::WebSocketMessageKind;

struct ConnectionPlan {
    std::optional<int> close_after_ready;
    bool block_first_command = false;
    bool mismatched_ready = false;
    bool block_first_binary = false;
    bool fail_first_binary = false;
};

struct ConnectionState {
    explicit ConnectionState(ConnectionPlan plan) : plan(std::move(plan)) {}

    ConnectionPlan plan;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<WebSocketMessage> inbound;
    std::vector<Json> sent;
    std::vector<std::string> binary_frames;
    bool command_blocked = false;
    bool release_command = false;
    bool binary_blocked = false;
    bool release_binary = false;
    bool binary_failed = false;
    std::optional<std::pair<int, std::string>> client_close;
};

Json ReadyEvent() {
    return {
        {"type", "session.ready"},
        {"sessionId", "fake-session-1"},
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
        {"microphone",
         {
             {"encoding", "pcm_s16le"},
             {"sampleRate", 16000},
             {"channels", 1},
             {"frameDurationMs", 20},
         }},
        {"health", "ready"},
        {"activity", "armed"},
    };
}

class FakeTransport final : public lva::cortana::SessionTransport {
public:
    explicit FakeTransport(std::shared_ptr<ConnectionState> state)
        : state_(std::move(state)) {}

    void SendText(std::string_view payload) override {
        const Json event = Json::parse(payload);
        std::unique_lock lock(state_->mutex);
        state_->sent.push_back(event);
        const std::string type = event.at("type").get<std::string>();
        if (type == "session.capabilities") {
            Json ready = ReadyEvent();
            if (state_->plan.mismatched_ready) {
                ready["satellite"]["areaId"] = "bedroom";
            }
            state_->inbound.push_back({
                .kind = WebSocketMessageKind::Text,
                .payload = ready.dump(),
                .close_code = 0,
                .close_reason = {},
            });
            if (state_->plan.close_after_ready.has_value()) {
                state_->inbound.push_back({
                    .kind = WebSocketMessageKind::Closed,
                    .payload = {},
                    .close_code = *state_->plan.close_after_ready,
                    .close_reason = *state_->plan.close_after_ready == 4001
                        ? "connection replaced"
                        : "temporary restart",
                });
            }
            state_->condition.notify_all();
        } else if (type == "session.ping") {
            Json health = {
                {"type", "session.health"},
                {"health", "ready"},
                {"activity", "armed"},
                {"nonce", event.at("nonce")},
            };
            state_->inbound.push_back({
                .kind = WebSocketMessageKind::Text,
                .payload = health.dump(),
                .close_code = 0,
                .close_reason = {},
            });
            state_->condition.notify_all();
        } else if (type == "test.command" &&
                   state_->plan.block_first_command &&
                   !state_->command_blocked) {
            state_->command_blocked = true;
            state_->condition.notify_all();
            state_->condition.wait(lock, [this] {
                return state_->release_command;
            });
        }
    }

    void SendBinary(std::string_view payload) override {
        std::unique_lock lock(state_->mutex);
        if (state_->plan.fail_first_binary && !state_->binary_failed) {
            state_->binary_blocked = true;
            state_->condition.notify_all();
            state_->condition.wait(lock, [this] {
                return state_->release_binary;
            });
            state_->binary_failed = true;
            throw lva::cortana::SessionTransportError(
                "scripted binary send failure");
        }
        if (state_->plan.block_first_binary && !state_->binary_blocked) {
            state_->binary_blocked = true;
            state_->condition.notify_all();
            state_->condition.wait(lock, [this] {
                return state_->release_binary;
            });
        }
        state_->binary_frames.emplace_back(payload);
        state_->condition.notify_all();
    }

    std::optional<WebSocketMessage> Receive(
        std::chrono::milliseconds timeout) override {
        std::unique_lock lock(state_->mutex);
        if (!state_->condition.wait_for(lock, timeout, [this] {
                return !state_->inbound.empty();
            })) {
            return std::nullopt;
        }
        WebSocketMessage result = std::move(state_->inbound.front());
        state_->inbound.pop_front();
        return result;
    }

    void Close(int code, std::string_view reason) noexcept override {
        std::lock_guard lock(state_->mutex);
        state_->client_close = std::pair(code, std::string(reason));
    }

private:
    std::shared_ptr<ConnectionState> state_;
};

class FakeDependencies final : public lva::cortana::SessionDependencies {
public:
    explicit FakeDependencies(std::vector<ConnectionPlan> plans)
        : plans_(std::move(plans)) {}

    lva::cortana::DeviceTicket RequestTicket(
        const lva::config::EndpointConfig&) override {
        std::lock_guard lock(mutex_);
        ++ticket_requests_;
        return {
            .ticket = std::string(31, 't') +
                static_cast<char>('0' + ticket_requests_),
            .expires_at = 2'000'000'000,
            .session_path = "/api/v1/voice/session",
            .protocol_version = "1",
            .satellite = {
                .satellite_id = "study-voice-1",
                .area_id = "study",
                .label = "Study voice endpoint",
            },
            .capabilities =
                lva::cortana::ContinuousDeviceCapabilities(),
        };
    }

    std::unique_ptr<lva::cortana::SessionTransport> Connect(
        std::string_view websocket_url) override {
        std::lock_guard lock(mutex_);
        urls_.emplace_back(websocket_url);
        const std::size_t index = states_.size();
        const ConnectionPlan plan = index < plans_.size()
            ? plans_[index]
            : ConnectionPlan{};
        auto state = std::make_shared<ConnectionState>(plan);
        states_.push_back(state);
        return std::make_unique<FakeTransport>(std::move(state));
    }

    int TicketRequests() const {
        std::lock_guard lock(mutex_);
        return ticket_requests_;
    }

    std::shared_ptr<ConnectionState> State(std::size_t index) const {
        std::lock_guard lock(mutex_);
        return index < states_.size() ? states_[index] : nullptr;
    }

    std::vector<std::string> Urls() const {
        std::lock_guard lock(mutex_);
        return urls_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<ConnectionPlan> plans_;
    std::vector<std::shared_ptr<ConnectionState>> states_;
    std::vector<std::string> urls_;
    int ticket_requests_ = 0;
};

lva::config::EndpointConfig Config() {
    return {
        .schema_version = 1,
        .endpoint = "https://cortana.example",
        .satellite_id = "study-voice-1",
        .expected_area_id = "study",
        .credential = std::string(40, 'c'),
    };
}

lva::cortana::SessionClient::Options FastOptions() {
    lva::cortana::SessionClient::Options options;
    options.maximum_queued_commands = 2;
    options.maximum_queued_events = 16;
    options.handshake_timeout = 100ms;
    options.receive_poll = 2ms;
    options.ping_interval = 1h;
    options.ping_timeout = 50ms;
    options.stable_connection_time = 1h;
    options.initial_backoff = 1ms;
    options.maximum_backoff = 4ms;
    return options;
}

std::unique_ptr<lva::cortana::SessionClient> MakeClient(
    const std::shared_ptr<FakeDependencies>& dependencies,
    lva::cortana::SessionClient::Options options = FastOptions()) {
    return std::make_unique<lva::cortana::SessionClient>(
        Config(), dependencies, std::move(options),
        [] { return lva::cortana::SessionClient::SteadyClock::now(); },
        [] { return 0.0; });
}

bool WaitUntil(std::function<bool()> predicate,
               std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

void TestHandshakeAndEndpoint() {
    auto dependencies =
        std::make_shared<FakeDependencies>(std::vector<ConnectionPlan>{{}});
    auto client = MakeClient(dependencies);
    client->Start();
    assert(client->WaitForPhase(SessionPhase::Ready, 1s));
    assert(WaitUntil([&] { return client->Snapshot().audio_started; }));
    const auto state = dependencies->State(0);
    assert(state);
    {
        std::lock_guard lock(state->mutex);
        assert(state->sent.size() >= 3);
        assert(state->sent[0].at("type") == "session.authenticate");
        assert(state->sent[1].at("type") == "session.capabilities");
        assert(state->sent[2].at("type") == "audio.start");
    }
    assert(dependencies->Urls() == std::vector<std::string>{
        "wss://cortana.example/api/v1/voice/session"});
    const auto event = client->TryPopEvent();
    assert(event.has_value());
    assert(std::holds_alternative<lva::cortana::SessionReady>(event->event));
    client->Stop();
}

void TestAudioGenerationMuteAndBackpressure() {
    auto options = FastOptions();
    options.maximum_queued_audio_frames = 2;
    auto dependencies = std::make_shared<FakeDependencies>(
        std::vector<ConnectionPlan>{{
            std::nullopt, false, false, true, false}});
    auto client = MakeClient(dependencies, options);
    client->Start();
    assert(client->WaitForPhase(SessionPhase::Ready, 1s));
    assert(WaitUntil([&] { return client->Snapshot().audio_started; }));
    const auto generation = client->Snapshot().generation;
    std::array<std::byte, lva::cortana::kMicrophoneFrameBytes> frame{};
    frame[0] = std::byte{0x11};
    assert(!client->EnqueueAudioFrame(generation + 1, frame));
    assert(client->EnqueueAudioFrame(generation, frame));

    const auto state = dependencies->State(0);
    assert(state);
    assert(WaitUntil([&] {
        std::lock_guard lock(state->mutex);
        return state->binary_blocked;
    }));
    assert(client->EnqueueAudioFrame(generation, frame));
    assert(client->EnqueueAudioFrame(generation, frame));
    assert(!client->EnqueueAudioFrame(generation, frame));
    client->SetMicrophoneMuted(true);
    assert(!client->EnqueueAudioFrame(generation, frame));
    assert(client->Snapshot().queued_audio_frames == 0);
    {
        std::lock_guard lock(state->mutex);
        state->release_binary = true;
        state->condition.notify_all();
    }
    client->Stop();
}

void TestReconnectDropsOldAudioGeneration() {
    auto dependencies = std::make_shared<FakeDependencies>(
        std::vector<ConnectionPlan>{
            {std::nullopt, false, false, false, true}, {}});
    auto client = MakeClient(dependencies);
    client->Start();
    assert(client->WaitForPhase(SessionPhase::Ready, 1s));
    assert(WaitUntil([&] { return client->Snapshot().audio_started; }));
    const auto first_generation = client->Snapshot().generation;
    std::array<std::byte, lva::cortana::kMicrophoneFrameBytes> frame{};
    frame[0] = std::byte{0x22};
    assert(client->EnqueueAudioFrame(first_generation, frame));
    const auto first = dependencies->State(0);
    assert(first);
    assert(WaitUntil([&] {
        std::lock_guard lock(first->mutex);
        return first->binary_blocked;
    }));
    assert(client->EnqueueAudioFrame(first_generation, frame));
    {
        std::lock_guard lock(first->mutex);
        first->release_binary = true;
        first->condition.notify_all();
    }
    assert(WaitUntil([&] {
        return client->Snapshot().generation > first_generation &&
            client->Snapshot().phase == SessionPhase::Ready &&
            client->Snapshot().audio_started;
    }));
    const auto second = dependencies->State(1);
    assert(second);
    std::this_thread::sleep_for(20ms);
    {
        std::lock_guard lock(second->mutex);
        assert(second->binary_frames.empty());
    }
    assert(!client->EnqueueAudioFrame(first_generation, frame));
    frame[0] = std::byte{0x33};
    assert(client->EnqueueAudioFrame(
        client->Snapshot().generation, frame));
    assert(WaitUntil([&] {
        std::lock_guard lock(second->mutex);
        return second->binary_frames.size() == 1;
    }));
    client->Stop();
}

void TestPersistentAudioOverloadReconnects() {
    auto options = FastOptions();
    options.maximum_queued_audio_frames = 2;
    options.maximum_audio_overload_strikes = 2;
    options.audio_overload_window = 1s;
    auto dependencies = std::make_shared<FakeDependencies>(
        std::vector<ConnectionPlan>{{
            std::nullopt, false, false, true, false}, {}});
    auto client = MakeClient(dependencies, options);
    client->Start();
    assert(client->WaitForPhase(SessionPhase::Ready, 1s));
    assert(WaitUntil([&] { return client->Snapshot().audio_started; }));
    const auto first_generation = client->Snapshot().generation;
    std::array<std::byte, lva::cortana::kMicrophoneFrameBytes> frame{};

    assert(client->EnqueueAudioFrame(first_generation, frame));
    const auto first = dependencies->State(0);
    assert(first);
    assert(WaitUntil([&] {
        std::lock_guard lock(first->mutex);
        return first->binary_blocked;
    }));

    for (int strike = 0; strike < 2; ++strike) {
        assert(client->EnqueueAudioFrame(first_generation, frame));
        assert(client->EnqueueAudioFrame(first_generation, frame));
        assert(!client->EnqueueAudioFrame(first_generation, frame));
        client->DiscardAudioFrames();
    }
    assert(client->Snapshot().audio_overload_reconnects == 1);

    {
        std::lock_guard lock(first->mutex);
        first->release_binary = true;
        first->condition.notify_all();
    }
    assert(WaitUntil([&] {
        const auto snapshot = client->Snapshot();
        return snapshot.generation > first_generation &&
            snapshot.phase == SessionPhase::Ready &&
            snapshot.audio_started;
    }));
    assert(dependencies->TicketRequests() >= 2);
    client->Stop();
}

void TestReconnectRefreshesTicket() {
    auto dependencies = std::make_shared<FakeDependencies>(
        std::vector<ConnectionPlan>{{1012, false}, {}});
    auto client = MakeClient(dependencies);
    client->Start();
    assert(WaitUntil([&] {
        return dependencies->TicketRequests() >= 2 &&
            client->Snapshot().phase == SessionPhase::Ready &&
            client->Snapshot().generation >= 2;
    }));
    const auto first = dependencies->State(0);
    const auto second = dependencies->State(1);
    assert(first && second);
    std::string first_ticket;
    std::string second_ticket;
    {
        std::lock_guard lock(first->mutex);
        first_ticket = first->sent.at(0).at("ticket").get<std::string>();
    }
    {
        std::lock_guard lock(second->mutex);
        second_ticket = second->sent.at(0).at("ticket").get<std::string>();
    }
    assert(first_ticket != second_ticket);
    client->Stop();
}

void TestReplacementCloseIsTerminal() {
    auto dependencies = std::make_shared<FakeDependencies>(
        std::vector<ConnectionPlan>{{4001, false}});
    auto client = MakeClient(dependencies);
    client->Start();
    assert(client->WaitForPhase(SessionPhase::Blocked, 1s));
    assert(client->Snapshot().detail.find("4001") != std::string::npos);
    std::this_thread::sleep_for(10ms);
    assert(dependencies->TicketRequests() == 1);
    client->Stop();
}

void TestNegotiatedIdentityMismatchIsTerminal() {
    auto dependencies = std::make_shared<FakeDependencies>(
        std::vector<ConnectionPlan>{{std::nullopt, false, true}});
    auto client = MakeClient(dependencies);
    client->Start();
    assert(client->WaitForPhase(SessionPhase::Blocked, 1s));
    assert(client->Snapshot().detail.find("do not match") !=
           std::string::npos);
    std::this_thread::sleep_for(10ms);
    assert(dependencies->TicketRequests() == 1);
    client->Stop();
}

void TestPingAndBoundedCommandQueue() {
    auto options = FastOptions();
    options.ping_interval = 5ms;
    auto dependencies = std::make_shared<FakeDependencies>(
        std::vector<ConnectionPlan>{{std::nullopt, true}});
    auto client = MakeClient(dependencies, options);
    client->Start();
    assert(client->WaitForPhase(SessionPhase::Ready, 1s));
    const auto state = dependencies->State(0);
    assert(state);
    assert(WaitUntil([&] {
        std::lock_guard lock(state->mutex);
        return std::any_of(state->sent.begin(), state->sent.end(),
                           [](const Json& event) {
                               return event.at("type") == "session.ping";
                           });
    }));

    assert(client->EnqueueText(R"({"type":"test.command","id":1})"));
    assert(WaitUntil([&] {
        std::lock_guard lock(state->mutex);
        return state->command_blocked;
    }));
    assert(client->EnqueueText(R"({"type":"test.command","id":2})"));
    assert(client->EnqueueText(R"({"type":"test.command","id":3})"));
    assert(!client->EnqueueText(R"({"type":"test.command","id":4})"));
    assert(client->Snapshot().dropped_commands == 1);
    {
        std::lock_guard lock(state->mutex);
        state->release_command = true;
        state->condition.notify_all();
    }
    client->Stop();
}

void TestBackoffAndUrlValidation() {
    auto options = FastOptions();
    options.initial_backoff = 100ms;
    options.maximum_backoff = 800ms;
    assert(lva::cortana::SessionClient::ReconnectDelay(0, options, 0.0) ==
           50ms);
    assert(lva::cortana::SessionClient::ReconnectDelay(3, options, 1.0) ==
           800ms);
    assert(lva::cortana::SessionClient::ReconnectDelay(20, options, 0.5) ==
           600ms);
    assert(lva::cortana::BuildWebSocketUrl(
               "https://cortana.example", "/api/v1/voice/session") ==
           "wss://cortana.example/api/v1/voice/session");
    bool rejected = false;
    try {
        (void)lva::cortana::BuildWebSocketUrl(
            "http://cortana.example", "/api/v1/voice/session");
    } catch (const lva::cortana::SessionTransportError&) {
        rejected = true;
    }
    assert(rejected);
}

}  // namespace

int main() {
    TestHandshakeAndEndpoint();
    TestAudioGenerationMuteAndBackpressure();
    TestReconnectDropsOldAudioGeneration();
    TestPersistentAudioOverloadReconnects();
    TestReconnectRefreshesTicket();
    TestReplacementCloseIsTerminal();
    TestNegotiatedIdentityMismatchIsTerminal();
    TestPingAndBoundedCommandQueue();
    TestBackoffAndUrlValidation();
}
