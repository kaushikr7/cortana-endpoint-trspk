#!/usr/bin/env python3
"""Provision Wi-Fi credentials over USB ADB without command-line secrets."""

from __future__ import annotations

import argparse
import getpass
import secrets
import subprocess
import sys
from typing import Sequence

from provision_cortana_endpoint import (
    AdbTarget,
    ProvisionError,
    cleanup_remote,
    run_adb_shell,
    write_remote_temp,
)


WIFI_CONNECT = "/usr/share/thirdreality/script/wifi_connect"


def validate_ssid(value: str) -> bytes:
    encoded = value.encode("utf-8")
    if not 1 <= len(encoded) <= 32:
        raise ProvisionError("SSID must contain between 1 and 32 UTF-8 bytes")
    if any(ord(character) < 32 or ord(character) == 127 for character in value):
        raise ProvisionError("SSID must not contain control characters")
    if '"' in value or "\\" in value:
        raise ProvisionError("SSID cannot contain quotes or backslashes")
    return encoded


def validate_password(value: str, open_network: bool) -> bytes:
    if open_network:
        if value:
            raise ProvisionError("an open network cannot have a password")
        return b""
    try:
        encoded = value.encode("ascii", "strict")
    except UnicodeEncodeError as error:
        raise ProvisionError("Wi-Fi password must contain printable ASCII") from error
    if not 8 <= len(encoded) <= 63:
        raise ProvisionError("Wi-Fi password must contain 8 to 63 characters")
    if any(byte < 32 or byte > 126 for byte in encoded):
        raise ProvisionError("Wi-Fi password must contain printable ASCII")
    if b'"' in encoded or b"\\" in encoded:
        raise ProvisionError("Wi-Fi password cannot contain quotes or backslashes")
    return encoded


def read_password(open_network: bool) -> bytes:
    if open_network:
        return b""
    if sys.stdin.isatty():
        value = getpass.getpass("Wi-Fi password: ")
    else:
        value = sys.stdin.read(65)
        value = value.removesuffix("\r\n").removesuffix("\n")
    return validate_password(value, False)


def provision(args: argparse.Namespace) -> int:
    ssid = validate_ssid(args.ssid)
    password = read_password(args.open_network)
    target = AdbTarget(args.adb, args.serial)
    token = secrets.token_hex(8)
    ssid_path = f"/tmp/wifi-provision-{token}.ssid"
    password_path = f"/tmp/wifi-provision-{token}.psk"
    paths = (ssid_path, password_path)
    try:
        write_remote_temp(target, ssid_path, ssid)
        write_remote_temp(target, password_path, password)
        run_adb_shell(
            target,
            f'SSID="$(cat {ssid_path})"; PSK="$(cat {password_path})"; '
            f'rm -f {ssid_path} {password_path}; '
            f'exec {WIFI_CONNECT} connect "$SSID" "$PSK"',
        )
    finally:
        cleanup_remote(target, paths)
    print(f"Provisioned Wi-Fi network {args.ssid}")
    return 0


def status(args: argparse.Namespace) -> int:
    completed = run_adb_shell(
        AdbTarget(args.adb, args.serial),
        "wpa_cli -i wlan0 status",
        check=False,
    )
    return completed.returncode


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Provision Wi-Fi over USB ADB. The password is read from a "
            "no-echo prompt or standard input and is never an argument."
        )
    )
    commands = parser.add_subparsers(dest="command", required=True)
    provision_parser = commands.add_parser("provision")
    provision_parser.add_argument("--adb", default="adb")
    provision_parser.add_argument("--serial")
    provision_parser.add_argument("--ssid", required=True)
    provision_parser.add_argument("--open-network", action="store_true")
    provision_parser.set_defaults(handler=provision)
    status_parser = commands.add_parser("status")
    status_parser.add_argument("--adb", default="adb")
    status_parser.add_argument("--serial")
    status_parser.set_defaults(handler=status)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.handler(args)
    except ProvisionError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except FileNotFoundError as error:
        print(f"error: executable not found: {error.filename}", file=sys.stderr)
        return 127
    except subprocess.CalledProcessError as error:
        print(
            f"error: ADB command failed with exit code {error.returncode}",
            file=sys.stderr,
        )
        return error.returncode or 1


if __name__ == "__main__":
    raise SystemExit(main())
