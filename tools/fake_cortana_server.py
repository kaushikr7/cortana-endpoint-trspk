#!/usr/bin/env python3
"""Protocol-faithful Cortana v1 fake session and optional WebSocket server."""

from __future__ import annotations

import argparse
import asyncio
import json
import re
from dataclasses import dataclass, field
from typing import Any, Iterable, Sequence


PROTOCOL_VERSION = "1"
FRAME_BYTES = 640
IDENTIFIER_RE = re.compile(r"^[A-Za-z0-9._:-]{1,100}$")
CANCELLATION_SOURCES = {"physical", "voice", "mute", "session"}
CAPABILITIES: dict[str, Any] = {
    "endpointKind": "device",
    "captureMode": "continuous",
    "wakeMode": "server",
    "microphone": {
        "encoding": "pcm_s16le",
        "sampleRate": 16000,
        "channels": 1,
        "frameDurationMs": 20,
    },
    "playback": True,
    "localPreRollMs": 0,
    "followUpCapture": True,
    "playbackAcknowledgements": True,
    "bargeInMode": "none",
}


class InvalidEvent(ValueError):
    pass


class FakeClose(RuntimeError):
    def __init__(self, code: int, reason: str) -> None:
        super().__init__(reason)
        self.code = code
        self.reason = reason


def require_object(value: Any, required: Iterable[str], optional: Iterable[str] = ()) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise InvalidEvent("event must be an object")
    required_set = set(required)
    allowed = required_set | set(optional)
    if missing := required_set - value.keys():
        raise InvalidEvent(f"missing fields: {sorted(missing)}")
    if extra := value.keys() - allowed:
        raise InvalidEvent(f"unknown fields: {sorted(extra)}")
    return value


def require_identifier(value: Any, field_name: str) -> str:
    if not isinstance(value, str) or not IDENTIFIER_RE.fullmatch(value):
        raise InvalidEvent(f"invalid {field_name}")
    return value


def require_reason(value: Any) -> str:
    reason = require_identifier(value, "reason")
    if len(reason) > 80:
        raise InvalidEvent("invalid reason")
    return reason


def error_event(code: str, message: str, recoverable: bool = True) -> dict[str, Any]:
    return {
        "type": "error",
        "error": {"code": code, "message": message, "recoverable": recoverable},
    }


@dataclass
class FakeSession:
    ticket: str = "t" * 32
    satellite_id: str = "study-voice-1"
    area_id: str = "study"
    label: str = "Study voice endpoint"
    maximum_queued_frames: int = 8
    stage: str = "authenticate"
    capture_started: bool = False
    muted: bool = False
    active_turn_id: str | None = None
    queued_frames: int = 0
    received_bytes: int = 0
    _turn_counter: int = field(default=0, init=False)

    def receive_text(self, payload: str) -> list[dict[str, Any]]:
        try:
            value = json.loads(payload)
        except json.JSONDecodeError as error:
            if self.stage == "authenticate":
                raise FakeClose(4401, "authentication rejected") from error
            if self.stage == "capabilities":
                raise FakeClose(4403, "capabilities rejected") from error
            return [error_event("invalid_event", "Unsupported voice event")]

        if self.stage == "authenticate":
            return self._authenticate(value)
        if self.stage == "capabilities":
            return self._capabilities(value)
        return self._client_event(value)

    def receive_binary(self, payload: bytes) -> list[dict[str, Any]]:
        if self.stage != "ready" or not self.capture_started:
            return [
                error_event(
                    "capture_inactive", "Audio requires a matching audio.start"
                )
            ]
        if len(payload) != FRAME_BYTES:
            return [error_event("invalid_audio_frame", "Audio frame must be 640 bytes")]
        if self.queued_frames >= self.maximum_queued_frames:
            return [error_event("audio_overloaded", "Audio ingress is at capacity")]
        self.queued_frames += 1
        self.received_bytes += len(payload)
        return []

    def drain_frames(self, count: int | None = None) -> None:
        if count is None:
            self.queued_frames = 0
        else:
            self.queued_frames = max(0, self.queued_frames - count)

    def _authenticate(self, value: Any) -> list[dict[str, Any]]:
        try:
            event = require_object(
                value, {"type", "protocolVersion", "ticket"}
            )
            if (
                event["type"] != "session.authenticate"
                or event["protocolVersion"] != PROTOCOL_VERSION
                or not isinstance(event["ticket"], str)
                or event["ticket"] != self.ticket
            ):
                raise InvalidEvent("authentication rejected")
        except InvalidEvent as error:
            raise FakeClose(4401, "authentication rejected") from error
        self.stage = "capabilities"
        return []

    def _capabilities(self, value: Any) -> list[dict[str, Any]]:
        try:
            event = require_object(value, {"type", "capabilities"})
            if (
                event["type"] != "session.capabilities"
                or event["capabilities"] != CAPABILITIES
            ):
                raise InvalidEvent("capabilities rejected")
        except InvalidEvent as error:
            raise FakeClose(4403, "capabilities rejected") from error
        self.stage = "ready"
        return [self.session_ready()]

    def session_ready(self) -> dict[str, Any]:
        return {
            "type": "session.ready",
            "sessionId": "fake-session-1",
            "protocolVersion": PROTOCOL_VERSION,
            "satellite": {
                "satelliteId": self.satellite_id,
                "areaId": self.area_id,
                "label": self.label,
            },
            "capabilities": CAPABILITIES,
            "microphone": CAPABILITIES["microphone"],
            "health": "ready",
            "activity": "armed",
        }

    def _client_event(self, value: Any) -> list[dict[str, Any]]:
        try:
            if not isinstance(value, dict) or not isinstance(value.get("type"), str):
                raise InvalidEvent("missing event type")
            event_type = value["type"]
            if event_type == "audio.start":
                event = require_object(
                    value,
                    {"type", "encoding", "sampleRate", "channels", "frameDurationMs"},
                    {"activationId"},
                )
                expected = {
                    "encoding": "pcm_s16le",
                    "sampleRate": 16000,
                    "channels": 1,
                    "frameDurationMs": 20,
                }
                if any(event[name] != expected[name] for name in expected):
                    raise InvalidEvent("unsupported microphone format")
                if "activationId" in event:
                    require_identifier(event["activationId"], "activationId")
                self.capture_started = True
                return []
            if event_type == "audio.stop":
                event = require_object(value, {"type"}, {"activationId"})
                if "activationId" in event:
                    require_identifier(event["activationId"], "activationId")
                self.capture_started = False
                self.queued_frames = 0
                return []
            if event_type == "session.ping":
                event = require_object(value, {"type"}, {"nonce"})
                nonce = event.get("nonce")
                if nonce is not None and (
                    not isinstance(nonce, str) or len(nonce) > 80
                ):
                    raise InvalidEvent("invalid nonce")
                result = {
                    "type": "session.health",
                    "health": "muted" if self.muted else "ready",
                    "activity": "armed" if self.active_turn_id is None else "hearing",
                }
                if nonce is not None:
                    result["nonce"] = nonce
                return [result]
            if event_type == "mute.changed":
                event = require_object(value, {"type", "muted"})
                if not isinstance(event["muted"], bool):
                    raise InvalidEvent("muted must be boolean")
                self.muted = event["muted"]
                responses: list[dict[str, Any]] = []
                if self.muted and self.active_turn_id is not None:
                    responses.append(
                        {
                            "type": "turn.cancelled",
                            "turnId": self.active_turn_id,
                            "source": "mute",
                            "reason": "microphone_muted",
                        }
                    )
                    self.active_turn_id = None
                responses.append(
                    {
                        "type": "session.health",
                        "health": "muted" if self.muted else "ready",
                        "activity": "armed",
                    }
                )
                return responses
            if event_type == "wake.manual":
                event = require_object(
                    value, {"type", "activationId"}, {"interruptsTurnId"}
                )
                activation_id = require_identifier(
                    event["activationId"], "activationId"
                )
                if "interruptsTurnId" in event:
                    require_identifier(event["interruptsTurnId"], "interruptsTurnId")
                if self.active_turn_id is not None:
                    return [
                        {
                            "type": "wake.suppressed",
                            "activationId": activation_id,
                            "reason": "satellite_busy",
                        }
                    ]
                self._turn_counter += 1
                self.active_turn_id = f"turn-{self._turn_counter}"
                return [
                    {
                        "type": "wake.accepted",
                        "activationId": activation_id,
                        "turnId": self.active_turn_id,
                    }
                ]
            if event_type == "turn.cancel":
                event = require_object(
                    value, {"type"}, {"turnId", "source", "reason"}
                )
                requested_turn = event.get("turnId")
                if requested_turn is not None:
                    require_identifier(requested_turn, "turnId")
                if requested_turn is not None and requested_turn != self.active_turn_id:
                    return [error_event("turn_unknown", "Cancellation does not match the active turn")]
                source = event.get("source", "physical")
                reason = event.get("reason", "user_cancelled")
                if source not in CANCELLATION_SOURCES:
                    raise InvalidEvent("invalid cancellation source")
                require_reason(reason)
                cancelled = self.active_turn_id
                self.active_turn_id = None
                result: dict[str, Any] = {"type": "turn.cancelled"}
                if cancelled is not None:
                    result["turnId"] = cancelled
                result["source"] = source
                result["reason"] = reason
                return [
                    result,
                    {
                        "type": "session.health",
                        "health": "muted" if self.muted else "ready",
                        "activity": "armed",
                    },
                ]
            if event_type in {
                "playback.started",
                "playback.completed",
                "playback.stopped",
            }:
                optional = {"reason"} if event_type == "playback.stopped" else set()
                event = require_object(value, {"type", "turnId"}, optional)
                turn_id = require_identifier(event["turnId"], "turnId")
                if "reason" in event:
                    require_reason(event["reason"])
                if turn_id != self.active_turn_id:
                    return [error_event("playback_unknown", "Playback does not match an active response")]
                return []
            raise InvalidEvent("unsupported event type")
        except InvalidEvent:
            return [error_event("invalid_event", "Unsupported voice event")]


async def run_websocket_server(args: argparse.Namespace) -> None:
    try:
        from websockets.asyncio.server import serve
    except ImportError as error:
        raise SystemExit(
            "Install the optional fake-server dependency: pip install 'websockets>=13,<16'"
        ) from error

    async def connection(websocket: Any) -> None:
        session = FakeSession(
            ticket=args.ticket,
            satellite_id=args.satellite_id,
            area_id=args.area_id,
            label=args.label,
        )
        try:
            async for message in websocket:
                try:
                    responses = (
                        session.receive_text(message)
                        if isinstance(message, str)
                        else session.receive_binary(message)
                    )
                except FakeClose as close:
                    await websocket.close(code=close.code, reason=close.reason)
                    return
                for response in responses:
                    await websocket.send(json.dumps(response, separators=(",", ":")))
                session.drain_frames(1)
        finally:
            session.drain_frames()

    async with serve(connection, args.host, args.port):
        print(f"Fake Cortana WebSocket listening on ws://{args.host}:{args.port}")
        await asyncio.Future()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--ticket", default="t" * 32)
    parser.add_argument("--satellite-id", default="study-voice-1")
    parser.add_argument("--area-id", default="study")
    parser.add_argument("--label", default="Study voice endpoint")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    asyncio.run(run_websocket_server(args))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
