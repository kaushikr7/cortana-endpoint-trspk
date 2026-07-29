from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import provision_cortana_endpoint as provision  # noqa: E402


class ValidationTests(unittest.TestCase):
    def test_valid_endpoint_is_normalized(self) -> None:
        self.assertEqual(
            provision.validate_endpoint("https://cortana.example.com/"),
            "https://cortana.example.com",
        )

    def test_endpoint_must_be_https_origin(self) -> None:
        invalid = (
            "http://cortana.example.com",
            "https://user@cortana.example.com",
            "https://cortana.example.com/api",
            "https://cortana.example.com:70000",
        )
        for endpoint in invalid:
            with self.subTest(endpoint=endpoint):
                with self.assertRaises(provision.ProvisionError):
                    provision.validate_endpoint(endpoint)

    def test_satellite_ids_match_the_ticket_endpoint_contract(self) -> None:
        self.assertEqual(
            provision.validate_satellite_id("study-voice-1"),
            "study-voice-1",
        )
        for identifier in (
            "",
            "Study",
            "1-study",
            "study_voice",
            "study.voice",
            "x" * 65,
        ):
            with self.subTest(identifier=identifier):
                with self.assertRaises(provision.ProvisionError):
                    provision.validate_satellite_id(identifier)

    def test_area_ids_use_server_area_grammar(self) -> None:
        self.assertEqual(provision.validate_area_id("guest_bathroom"),
                         "guest_bathroom")
        for identifier in ("", "Guest", "1study", "guest-bathroom"):
            with self.subTest(identifier=identifier):
                with self.assertRaises(provision.ProvisionError):
                    provision.validate_area_id(identifier)

    def test_credential_is_printable_and_long_enough(self) -> None:
        credential = b"a" * 32
        self.assertIs(provision.validate_credential(credential), credential)
        for invalid in (b"short", b"a" * 31 + b"\n", b"a" * 4097):
            with self.subTest(length=len(invalid)):
                with self.assertRaises(provision.ProvisionError):
                    provision.validate_credential(invalid)


class AdbCommandTests(unittest.TestCase):
    def test_serial_is_passed_as_separate_adb_arguments(self) -> None:
        target = provision.AdbTarget("/opt/android/adb", "device-1")
        self.assertEqual(
            target.command("shell", "true"),
            ["/opt/android/adb", "-s", "device-1", "shell", "true"],
        )

    @mock.patch.object(provision.subprocess, "run")
    def test_credential_bytes_are_stdin_not_command_arguments(
        self, run: mock.Mock
    ) -> None:
        run.return_value = subprocess.CompletedProcess([], 0)
        credential = b"secret-value-that-is-long-enough-123"
        provision.write_remote_temp(
            provision.AdbTarget(),
            "/data/cortana/.credential.test.tmp",
            credential,
        )
        command = run.call_args.args[0]
        self.assertNotIn(credential.decode(), " ".join(command))
        self.assertEqual(run.call_args.kwargs["input"], credential)

    @mock.patch.object(provision, "run_adb_shell")
    def test_endpoint_restart_uses_legacy_service_name(
        self, run_shell: mock.Mock
    ) -> None:
        target = provision.AdbTarget(serial="device-1")
        provision.restart_endpoint(target)
        run_shell.assert_called_once_with(
            target,
            "/etc/init.d/S99ha-speaker voice-assistant restart",
        )


if __name__ == "__main__":
    unittest.main()
