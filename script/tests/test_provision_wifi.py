from __future__ import annotations

import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import provision_wifi as wifi  # noqa: E402
from provision_cortana_endpoint import ProvisionError  # noqa: E402


class WifiValidationTests(unittest.TestCase):
    def test_ssid_uses_wifi_byte_limit(self) -> None:
        self.assertEqual(wifi.validate_ssid("satori"), b"satori")
        self.assertEqual(wifi.validate_ssid("\u732b" * 10), ("\u732b" * 10).encode())
        for invalid in ("", "x" * 33, "bad\nssid", "bad\\ssid", 'bad"ssid', "\u732b" * 11):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ProvisionError):
                    wifi.validate_ssid(invalid)

    def test_wpa_password_is_printable_ascii(self) -> None:
        self.assertEqual(wifi.validate_password("password", False), b"password")
        for invalid in (
            "short",
            "x" * 64,
            "bad\npassword",
            "bad\\password",
            'bad"password',
            "p\u00e4ssword",
        ):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ProvisionError):
                    wifi.validate_password(invalid, False)

    def test_open_network_has_no_password(self) -> None:
        self.assertEqual(wifi.validate_password("", True), b"")
        with self.assertRaises(ProvisionError):
            wifi.validate_password("password", True)


if __name__ == "__main__":
    unittest.main()
