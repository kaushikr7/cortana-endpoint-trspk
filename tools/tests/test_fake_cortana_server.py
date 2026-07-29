from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from fake_cortana_server import CAPABILITIES, FakeClose, FakeSession  # noqa: E402


class FakeSessionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.session = FakeSession(maximum_queued_frames=1)

    def authenticate(self) -> dict:
        self.assertEqual(
            self.session.receive_text(
                json.dumps(
                    {
                        "type": "session.authenticate",
                        "protocolVersion": "1",
                        "ticket": "t" * 32,
                    }
                )
            ),
            [],
        )
        ready = self.session.receive_text(
            json.dumps(
                {"type": "session.capabilities", "capabilities": CAPABILITIES}
            )
        )
        self.assertEqual(len(ready), 1)
        return ready[0]

    def test_strict_handshake_returns_authoritative_ready(self) -> None:
        ready = self.authenticate()
        self.assertEqual(ready["type"], "session.ready")
        self.assertEqual(ready["satellite"]["areaId"], "study")
        self.assertEqual(ready["capabilities"], CAPABILITIES)
        self.assertEqual(ready["microphone"], CAPABILITIES["microphone"])

    def test_authentication_and_capability_failures_use_server_close_codes(self) -> None:
        with self.assertRaises(FakeClose) as authentication:
            self.session.receive_text(
                json.dumps(
                    {
                        "type": "session.authenticate",
                        "protocolVersion": "1",
                        "ticket": "wrong" * 8,
                    }
                )
            )
        self.assertEqual(authentication.exception.code, 4401)

        session = FakeSession()
        session.receive_text(
            json.dumps(
                {
                    "type": "session.authenticate",
                    "protocolVersion": "1",
                    "ticket": "t" * 32,
                }
            )
        )
        mismatch = dict(CAPABILITIES)
        mismatch["wakeMode"] = "satellite"
        with self.assertRaises(FakeClose) as capabilities:
            session.receive_text(
                json.dumps(
                    {"type": "session.capabilities", "capabilities": mismatch}
                )
            )
        self.assertEqual(capabilities.exception.code, 4403)

    def test_unknown_or_extra_fields_are_rejected(self) -> None:
        self.authenticate()
        response = self.session.receive_text(
            json.dumps({"type": "session.ping", "nonce": "x", "extra": True})
        )
        self.assertEqual(response[0]["error"]["code"], "invalid_event")
        response = self.session.receive_text(json.dumps({"type": "future.event"}))
        self.assertEqual(response[0]["error"]["code"], "invalid_event")

    def test_audio_requires_start_exact_frames_and_bounded_queue(self) -> None:
        self.authenticate()
        self.assertEqual(
            self.session.receive_binary(b"\0" * 640)[0]["error"]["code"],
            "capture_inactive",
        )
        self.session.receive_text(
            json.dumps(
                {
                    "type": "audio.start",
                    "encoding": "pcm_s16le",
                    "sampleRate": 16000,
                    "channels": 1,
                    "frameDurationMs": 20,
                }
            )
        )
        self.assertEqual(
            self.session.receive_binary(b"\0" * 639)[0]["error"]["code"],
            "invalid_audio_frame",
        )
        self.assertEqual(self.session.receive_binary(b"\0" * 640), [])
        self.assertEqual(
            self.session.receive_binary(b"\0" * 640)[0]["error"]["code"],
            "audio_overloaded",
        )

    def test_manual_wake_cancel_ping_and_playback_ordering(self) -> None:
        self.authenticate()
        accepted = self.session.receive_text(
            json.dumps({"type": "wake.manual", "activationId": "manual-1"})
        )[0]
        self.assertEqual(accepted["type"], "wake.accepted")
        turn_id = accepted["turnId"]

        self.assertEqual(
            self.session.receive_text(
                json.dumps({"type": "playback.started", "turnId": turn_id})
            ),
            [],
        )
        unknown = self.session.receive_text(
            json.dumps({"type": "playback.completed", "turnId": "other-turn"})
        )
        self.assertEqual(unknown[0]["error"]["code"], "playback_unknown")

        cancelled = self.session.receive_text(
            json.dumps(
                {
                    "type": "turn.cancel",
                    "turnId": turn_id,
                    "source": "physical",
                    "reason": "user_cancelled",
                }
            )
        )
        self.assertEqual(cancelled[0]["type"], "turn.cancelled")
        health = self.session.receive_text(
            json.dumps({"type": "session.ping", "nonce": "check"})
        )[0]
        self.assertEqual(health["nonce"], "check")


if __name__ == "__main__":
    unittest.main()
