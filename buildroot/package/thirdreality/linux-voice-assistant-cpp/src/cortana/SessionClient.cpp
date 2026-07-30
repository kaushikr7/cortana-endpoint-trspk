#include "cortana/SessionClient.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace lva::cortana {

namespace {

class TerminalSessionError : public SessionTransportError {
public:
    using SessionTransportError::SessionTransportError;
};

class ClosedSessionError : public SessionTransportError {
public:
    ClosedSessionError(int code, std::string reason)
        : SessionTransportError(
              "WebSocket closed with code " + std::to_string(code) +
              (reason.empty() ? std::string{} : ": " + reason)),
          code(code) {}

    int code;
};

double DefaultJitter() {
    thread_local std::mt19937 generator(std::random_device{}());
    thread_local std::uniform_real_distribution<double> distribution(0.0, 1.0);
    return distribution(generator);
}

bool TicketFailureIsTerminal(TicketErrorCode code) {
    switch (code) {
        case TicketErrorCode::AuthenticationRejected:
        case TicketErrorCode::InvalidResponse:
        case TicketErrorCode::IdentityMismatch:
        case TicketErrorCode::CapabilityMismatch:
            return true;
        default:
            return false;
    }
}

bool CloseIsTerminal(int code) {
    return code == 4001 || code == 4401 || code == 4403;
}

std::chrono::milliseconds Remaining(
    SessionClient::SteadyClock::time_point now,
    SessionClient::SteadyClock::time_point deadline,
    std::chrono::milliseconds maximum) {
    if (deadline <= now) return std::chrono::milliseconds::zero();
    return std::min(
        maximum,
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
}

}  // namespace

std::string BuildWebSocketUrl(std::string_view https_origin,
                              std::string_view session_path) {
    if (!https_origin.starts_with("https://") ||
        https_origin.size() <= std::string_view("https://").size() ||
        https_origin.back() == '/' ||
        session_path != "/api/v1/voice/session") {
        throw SessionTransportError("invalid Cortana WebSocket endpoint");
    }
    return "wss" + std::string(https_origin.substr(5)) +
        std::string(session_path);
}

SessionClient::SessionClient(
    lva::config::EndpointConfig config,
    std::shared_ptr<SessionDependencies> dependencies)
    : SessionClient(
          std::move(config), std::move(dependencies), Options{},
          [] { return SteadyClock::now(); }, [] { return DefaultJitter(); }) {}

SessionClient::SessionClient(
    lva::config::EndpointConfig config,
    std::shared_ptr<SessionDependencies> dependencies,
    Options options,
    ClockFn clock,
    JitterFn jitter)
    : config_(std::move(config)),
      dependencies_(std::move(dependencies)),
      options_(std::move(options)),
      clock_(std::move(clock)),
      jitter_(std::move(jitter)) {
    if (!dependencies_ || !clock_ || !jitter_ ||
        options_.maximum_queued_commands == 0 ||
        options_.maximum_queued_events == 0 ||
        options_.maximum_queued_audio_frames == 0 ||
        options_.maximum_audio_overload_strikes == 0 ||
        options_.audio_overload_window <= std::chrono::milliseconds::zero() ||
        options_.maximum_command_bytes == 0 ||
        options_.handshake_timeout <= std::chrono::milliseconds::zero() ||
        options_.receive_poll <= std::chrono::milliseconds::zero() ||
        options_.ping_interval <= std::chrono::milliseconds::zero() ||
        options_.ping_timeout <= std::chrono::milliseconds::zero() ||
        options_.initial_backoff <= std::chrono::milliseconds::zero() ||
        options_.maximum_backoff < options_.initial_backoff) {
        throw std::invalid_argument("invalid Cortana session options");
    }
}

SessionClient::~SessionClient() {
    Stop();
}

void SessionClient::Start() {
    std::lock_guard lock(mutex_);
    if (worker_.joinable()) return;
    stop_requested_ = false;
    snapshot_ = SessionSnapshot{};
    commands_.clear();
    audio_frames_.clear();
    audio_overload_strikes_ = 0;
    last_audio_overload_ = SteadyClock::time_point{};
    audio_reconnect_requested_ = false;
    events_.clear();
    worker_ = std::thread([this] { Run(); });
}

void SessionClient::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (!worker_.joinable()) {
            snapshot_.phase = SessionPhase::Stopped;
            return;
        }
        stop_requested_ = true;
    }
    condition_.notify_all();
    worker_.join();
    {
        std::lock_guard lock(mutex_);
        snapshot_.phase = SessionPhase::Stopped;
        snapshot_.detail.clear();
        snapshot_.session_id.clear();
        snapshot_.audio_started = false;
    }
    condition_.notify_all();
}

bool SessionClient::EnqueueText(std::string payload) {
    std::lock_guard lock(mutex_);
    if (snapshot_.phase != SessionPhase::Ready || payload.empty() ||
        payload.size() > options_.maximum_command_bytes ||
        commands_.size() >= options_.maximum_queued_commands) {
        ++snapshot_.dropped_commands;
        return false;
    }
    commands_.push_back(std::move(payload));
    snapshot_.queued_commands = commands_.size();
    condition_.notify_all();
    return true;
}

bool SessionClient::EnqueueAudioFrame(
    std::uint64_t generation,
    const std::array<std::byte, kMicrophoneFrameBytes>& frame) {
    std::lock_guard lock(mutex_);
    if (snapshot_.phase != SessionPhase::Ready ||
        !snapshot_.audio_started || snapshot_.microphone_muted ||
        generation != snapshot_.generation) {
        ++snapshot_.dropped_audio_frames;
        return false;
    }
    if (audio_frames_.size() >= options_.maximum_queued_audio_frames) {
        ++snapshot_.dropped_audio_frames;
        const auto now = clock_();
        if (last_audio_overload_ == SteadyClock::time_point{} ||
            now - last_audio_overload_ > options_.audio_overload_window) {
            audio_overload_strikes_ = 0;
        }
        last_audio_overload_ = now;
        ++audio_overload_strikes_;
        if (!audio_reconnect_requested_ &&
            audio_overload_strikes_ >=
                options_.maximum_audio_overload_strikes) {
            audio_reconnect_requested_ = true;
            ++snapshot_.audio_overload_reconnects;
            DropQueuedAudioLocked();
            condition_.notify_all();
        }
        return false;
    }
    audio_frames_.push_back(frame);
    snapshot_.queued_audio_frames = audio_frames_.size();
    condition_.notify_all();
    return true;
}

void SessionClient::SetMicrophoneMuted(bool muted) {
    std::lock_guard lock(mutex_);
    snapshot_.microphone_muted = muted;
    if (muted) DropQueuedAudioLocked();
    condition_.notify_all();
}

void SessionClient::DiscardAudioFrames() {
    std::lock_guard lock(mutex_);
    DropQueuedAudioLocked();
}

std::optional<SessionEvent> SessionClient::TryPopEvent() {
    std::lock_guard lock(mutex_);
    if (events_.empty()) return std::nullopt;
    SessionEvent result = std::move(events_.front());
    events_.pop_front();
    return result;
}

SessionSnapshot SessionClient::Snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

bool SessionClient::WaitForPhase(
    SessionPhase phase, std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this, phase] {
        return snapshot_.phase == phase;
    });
}

std::chrono::milliseconds SessionClient::ReconnectDelay(
    std::size_t attempt,
    const Options& options,
    double jitter_unit) {
    jitter_unit = std::clamp(jitter_unit, 0.0, 1.0);
    const auto initial = options.initial_backoff.count();
    const auto maximum = options.maximum_backoff.count();
    std::int64_t capped = initial;
    for (std::size_t index = 0;
         index < attempt && capped < maximum;
         ++index) {
        capped = std::min<std::int64_t>(maximum, capped * 2);
    }
    const double equal_jitter = 0.5 + (0.5 * jitter_unit);
    return std::chrono::milliseconds(
        static_cast<std::int64_t>(std::llround(capped * equal_jitter)));
}

void SessionClient::Run() {
    std::size_t reconnect_attempt = 0;
    while (!StopRequested()) {
        const auto attempt_started = clock_();
        {
            std::lock_guard lock(mutex_);
            ++snapshot_.generation;
            snapshot_.reconnect_attempt = reconnect_attempt;
        }
        SetPhase(SessionPhase::Connecting,
                 reconnect_attempt == 0 ? "requesting ticket"
                                        : "refreshing ticket");

        try {
            const DeviceTicket ticket = dependencies_->RequestTicket(config_);
            if (StopRequested()) break;
            const std::string url =
                BuildWebSocketUrl(config_.endpoint, ticket.session_path);
            std::unique_ptr<SessionTransport> transport =
                dependencies_->Connect(url);
            if (!transport) {
                throw SessionTransportError(
                    "WebSocket transport factory returned no connection");
            }
            SetPhase(SessionPhase::Negotiating, "authenticating session");
            RunConnection(ticket, *transport);
            transport->Close(1000, "stopped");
            break;
        } catch (const TerminalSessionError& error) {
            DropQueuedCommands();
            SetPhase(SessionPhase::Blocked, error.what());
            return;
        } catch (const ClosedSessionError& error) {
            DropQueuedCommands();
            if (CloseIsTerminal(error.code)) {
                SetPhase(SessionPhase::Blocked, error.what());
                return;
            }
            SetPhase(SessionPhase::Backoff, error.what());
        } catch (const TicketError& error) {
            DropQueuedCommands();
            if (TicketFailureIsTerminal(error.code())) {
                SetPhase(SessionPhase::Blocked, error.what());
                return;
            }
            SetPhase(SessionPhase::Backoff, error.what());
        } catch (const std::exception& error) {
            DropQueuedCommands();
            SetPhase(SessionPhase::Backoff, error.what());
        }

        if (StopRequested()) break;
        const bool was_stable = clock_() - attempt_started >=
            options_.stable_connection_time;
        if (was_stable) reconnect_attempt = 0;
        const auto delay =
            ReconnectDelay(reconnect_attempt, options_, jitter_());
        {
            std::lock_guard lock(mutex_);
            snapshot_.reconnect_attempt = reconnect_attempt + 1;
        }
        ++reconnect_attempt;
        if (WaitForStop(delay)) break;
    }
    DropQueuedCommands();
    SetPhase(SessionPhase::Stopped);
}

void SessionClient::RunConnection(const DeviceTicket& ticket,
                                  SessionTransport& transport) {
    transport.SendText(SerializeSessionAuthenticate(ticket.ticket));
    transport.SendText(SerializeSessionCapabilities(ticket.capabilities));

    const auto handshake_deadline = clock_() + options_.handshake_timeout;
    auto next_ping = SteadyClock::time_point::max();
    auto ping_deadline = SteadyClock::time_point::max();
    std::optional<std::string> pending_ping;
    std::uint64_t ping_sequence = 0;
    bool ready = false;

    while (!StopRequested()) {
        {
            std::lock_guard lock(mutex_);
            if (audio_reconnect_requested_) {
                audio_reconnect_requested_ = false;
                audio_overload_strikes_ = 0;
                last_audio_overload_ = SteadyClock::time_point{};
                throw SessionTransportError(
                    "audio transport remained overloaded");
            }
        }
        if (ready) {
            std::optional<std::string> command;
            {
                std::lock_guard lock(mutex_);
                if (!commands_.empty()) {
                    command = std::move(commands_.front());
                    commands_.pop_front();
                    snapshot_.queued_commands = commands_.size();
                }
            }
            if (command.has_value()) transport.SendText(*command);

            for (int frame_index = 0; frame_index < 4; ++frame_index) {
                std::optional<std::array<std::byte, kMicrophoneFrameBytes>>
                    audio_frame;
                {
                    std::lock_guard lock(mutex_);
                    if (snapshot_.microphone_muted ||
                        !snapshot_.audio_started || audio_frames_.empty()) {
                        break;
                    }
                    audio_frame = std::move(audio_frames_.front());
                    audio_frames_.pop_front();
                    snapshot_.queued_audio_frames = audio_frames_.size();
                }
                {
                    std::lock_guard lock(mutex_);
                    if (snapshot_.microphone_muted ||
                        !snapshot_.audio_started) {
                        ++snapshot_.dropped_audio_frames;
                        continue;
                    }
                }
                try {
                    transport.SendBinary(std::string_view(
                        reinterpret_cast<const char*>(audio_frame->data()),
                        audio_frame->size()));
                } catch (...) {
                    std::lock_guard lock(mutex_);
                    ++snapshot_.dropped_audio_frames;
                    throw;
                }
                {
                    std::lock_guard lock(mutex_);
                    ++snapshot_.audio_frames_sent;
                }
            }
        }

        const auto before_receive = clock_();
        auto receive_timeout = options_.receive_poll;
        receive_timeout = Remaining(
            before_receive,
            ready ? next_ping : handshake_deadline,
            receive_timeout);
        if (pending_ping.has_value()) {
            receive_timeout = Remaining(
                before_receive, ping_deadline, receive_timeout);
        }
        {
            std::lock_guard lock(mutex_);
            if (ready && !audio_frames_.empty()) {
                receive_timeout = std::chrono::milliseconds::zero();
            }
        }

        const auto message = transport.Receive(receive_timeout);
        if (message.has_value()) {
            if (message->kind == WebSocketMessageKind::Closed) {
                throw ClosedSessionError(
                    message->close_code, message->close_reason);
            }
            if (message->kind != WebSocketMessageKind::Text) {
                transport.Close(1002, "unexpected binary message");
                throw SessionTransportError(
                    "unexpected binary message before playback support");
            }

            ServerEvent event;
            try {
                event = ParseServerEvent(message->payload);
            } catch (const ProtocolError& error) {
                transport.Close(1002, "invalid voice protocol");
                throw SessionTransportError(error.what());
            }

            if (!ready) {
                if (const auto* session_ready =
                        std::get_if<SessionReady>(&event)) {
                    SetReady(*session_ready, ticket);
                    transport.SendText(SerializeAudioStart());
                    {
                        std::lock_guard lock(mutex_);
                        snapshot_.audio_started = true;
                    }
                    condition_.notify_all();
                    ready = true;
                    next_ping = clock_() + options_.ping_interval;
                    PushEvent(std::move(event));
                } else if (const auto* rejected =
                               std::get_if<ErrorEvent>(&event)) {
                    throw TerminalSessionError(
                        "Cortana rejected session negotiation: " +
                        rejected->code);
                } else {
                    transport.Close(1002, "invalid handshake event");
                    throw SessionTransportError(
                        "Cortana sent an event before session.ready");
                }
            } else {
                if (std::holds_alternative<SessionReady>(event)) {
                    transport.Close(1002, "duplicate session.ready");
                    throw SessionTransportError(
                        "Cortana sent duplicate session.ready");
                }
                bool keepalive_reply = false;
                if (const auto* health = std::get_if<SessionHealth>(&event)) {
                    {
                        std::lock_guard lock(mutex_);
                        snapshot_.health = health->health;
                        snapshot_.activity = health->activity;
                    }
                    if (pending_ping.has_value() && health->nonce == pending_ping) {
                        keepalive_reply = true;
                        pending_ping.reset();
                        ping_deadline = SteadyClock::time_point::max();
                    }
                }
                const bool terminal_error =
                    std::holds_alternative<ErrorEvent>(event) &&
                    !std::get<ErrorEvent>(event).recoverable;
                if (!keepalive_reply) PushEvent(std::move(event));
                if (terminal_error) {
                    throw TerminalSessionError(
                        "Cortana reported a non-recoverable session error");
                }
            }
        }

        const auto now = clock_();
        if (!ready && now >= handshake_deadline) {
            transport.Close(1000, "handshake timeout");
            throw SessionTransportError("Cortana session handshake timed out");
        }
        if (pending_ping.has_value() && now >= ping_deadline) {
            transport.Close(1001, "ping timeout");
            throw SessionTransportError("Cortana session ping timed out");
        }
        if (ready && !pending_ping.has_value() && now >= next_ping) {
            const std::string nonce =
                std::to_string(Snapshot().generation) + "-" +
                std::to_string(++ping_sequence);
            transport.SendText(SerializeSessionPing(nonce));
            pending_ping = nonce;
            ping_deadline = now + options_.ping_timeout;
            next_ping = now + options_.ping_interval;
        }
    }
}

void SessionClient::SetPhase(SessionPhase phase, std::string detail) {
    {
        std::lock_guard lock(mutex_);
        snapshot_.phase = phase;
        snapshot_.detail = std::move(detail);
        if (phase != SessionPhase::Ready) {
            snapshot_.audio_started = false;
            DropQueuedAudioLocked();
            audio_overload_strikes_ = 0;
            last_audio_overload_ = SteadyClock::time_point{};
            audio_reconnect_requested_ = false;
            snapshot_.session_id.clear();
            snapshot_.health = phase == SessionPhase::Backoff
                ? Health::Reconnecting
                : (phase == SessionPhase::Blocked ? Health::Blocked
                                                   : Health::Starting);
            snapshot_.activity = Activity::Idle;
        }
    }
    condition_.notify_all();
}

void SessionClient::SetReady(const SessionReady& ready,
                             const DeviceTicket& ticket) {
    if (ready.satellite.satellite_id != ticket.satellite.satellite_id ||
        ready.satellite.area_id != ticket.satellite.area_id ||
        ready.capabilities != ticket.capabilities ||
        ready.microphone != ticket.capabilities.microphone ||
        ready.satellite.satellite_id != config_.satellite_id ||
        (!config_.expected_area_id.empty() &&
         ready.satellite.area_id != config_.expected_area_id)) {
        throw TerminalSessionError(
            "session.ready identity or capabilities do not match the ticket");
    }
    {
        std::lock_guard lock(mutex_);
        snapshot_.phase = SessionPhase::Ready;
        snapshot_.health = ready.health;
        snapshot_.activity = ready.activity;
        snapshot_.session_id = ready.session_id;
        snapshot_.detail.clear();
        audio_overload_strikes_ = 0;
        last_audio_overload_ = SteadyClock::time_point{};
        audio_reconnect_requested_ = false;
    }
    condition_.notify_all();
}

void SessionClient::PushEvent(ServerEvent event) {
    std::lock_guard lock(mutex_);
    if (events_.size() >= options_.maximum_queued_events) {
        throw SessionTransportError(
            "bounded Cortana event queue is full");
    }
    events_.push_back(SessionEvent{
        .generation = snapshot_.generation,
        .event = std::move(event),
    });
    condition_.notify_all();
}

void SessionClient::DropQueuedCommands() {
    std::lock_guard lock(mutex_);
    snapshot_.dropped_commands += commands_.size();
    commands_.clear();
    snapshot_.queued_commands = 0;
}

void SessionClient::DropQueuedAudioLocked() {
    snapshot_.dropped_audio_frames += audio_frames_.size();
    audio_frames_.clear();
    snapshot_.queued_audio_frames = 0;
}

bool SessionClient::WaitForStop(std::chrono::milliseconds duration) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, duration, [this] {
        return stop_requested_;
    });
}

bool SessionClient::StopRequested() const {
    std::lock_guard lock(mutex_);
    return stop_requested_;
}

std::string_view ToString(SessionPhase phase) {
    switch (phase) {
        case SessionPhase::Stopped: return "stopped";
        case SessionPhase::Connecting: return "connecting";
        case SessionPhase::Negotiating: return "negotiating";
        case SessionPhase::Ready: return "ready";
        case SessionPhase::Backoff: return "backoff";
        case SessionPhase::Blocked: return "blocked";
    }
    return "blocked";
}

}  // namespace lva::cortana
