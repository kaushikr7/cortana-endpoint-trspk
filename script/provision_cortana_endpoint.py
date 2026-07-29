#!/usr/bin/env python3
"""Provision Cortana endpoint identity and credentials over ADB/USB."""

from __future__ import annotations

import argparse
import getpass
import json
import os
import re
import secrets
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Sequence
from urllib.parse import urlsplit


REMOTE_DIR = "/data/cortana"
REMOTE_CONFIG = f"{REMOTE_DIR}/config.json"
REMOTE_CREDENTIAL = f"{REMOTE_DIR}/credential"
DEFAULT_ENDPOINT_BINARY = "/usr/bin/linux-voice-assistant-cpp"
ENDPOINT_SERVICE = "/etc/init.d/S99ha-speaker"
SATELLITE_ID_RE = re.compile(r"^[a-z][a-z0-9-]{0,63}$")
AREA_ID_RE = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
HOST_RE = re.compile(r"^[A-Za-z0-9.-]+$")
MIN_CREDENTIAL_BYTES = 32
MAX_CREDENTIAL_BYTES = 4096


class ProvisionError(RuntimeError):
    """An expected provisioning or validation failure."""


@dataclass(frozen=True)
class AdbTarget:
    executable: str = "adb"
    serial: str | None = None

    def command(self, *arguments: str) -> list[str]:
        command = [self.executable]
        if self.serial:
            command.extend(("-s", self.serial))
        command.extend(arguments)
        return command


def validate_satellite_id(value: str) -> str:
    if not SATELLITE_ID_RE.fullmatch(value):
        raise ProvisionError("satellite ID must match [a-z][a-z0-9-]{0,63}")
    return value


def validate_area_id(value: str) -> str:
    if not AREA_ID_RE.fullmatch(value):
        raise ProvisionError("expected area ID must match [a-z][a-z0-9_]{0,63}")
    return value


def validate_endpoint(value: str) -> str:
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError as error:
        raise ProvisionError("endpoint is not a valid HTTPS origin") from error
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path not in ("", "/")
        or parsed.query
        or parsed.fragment
        or not HOST_RE.fullmatch(parsed.hostname)
    ):
        raise ProvisionError(
            "endpoint must be an HTTPS origin without a path, query, or credentials"
        )
    if port is not None and not 1 <= port <= 65535:
        raise ProvisionError("endpoint port must be between 1 and 65535")
    return value.rstrip("/")


def validate_credential(value: bytes) -> bytes:
    if len(value) < MIN_CREDENTIAL_BYTES:
        raise ProvisionError("credential must contain at least 32 characters")
    if len(value) > MAX_CREDENTIAL_BYTES:
        raise ProvisionError("credential is too large")
    if any(byte < 33 or byte > 126 for byte in value):
        raise ProvisionError(
            "credential must contain printable ASCII without whitespace"
        )
    return value


def read_credential() -> bytes:
    if sys.stdin.isatty():
        value = getpass.getpass("Device credential: ").encode("ascii", "strict")
    else:
        value = sys.stdin.buffer.read(MAX_CREDENTIAL_BYTES + 2)
        if value.endswith(b"\r\n"):
            value = value[:-2]
        elif value.endswith(b"\n"):
            value = value[:-1]
    return validate_credential(value)


def run_adb_shell(
    target: AdbTarget,
    command: str,
    *,
    input_bytes: bytes | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        target.command("shell", command),
        input=input_bytes,
        check=check,
    )


def adb_local_path(target: AdbTarget, path: str) -> str:
    """Translate a WSL path when invoking a Windows adb.exe."""
    if sys.platform.startswith("linux") and target.executable.lower().endswith(
        ".exe"
    ):
        completed = subprocess.run(
            ("wslpath", "-w", path),
            check=True,
            capture_output=True,
            text=True,
        )
        return completed.stdout.strip()
    return path


def prepare_remote_directory(target: AdbTarget) -> None:
    run_adb_shell(
        target,
        f"umask 077; mkdir -p {REMOTE_DIR} && chmod 0700 {REMOTE_DIR}",
    )


def write_remote_temp(target: AdbTarget, path: str, content: bytes) -> None:
    descriptor, local_path = tempfile.mkstemp(prefix="cortana-provision-")
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb") as local_file:
            local_file.write(content)
            local_file.flush()
            os.fsync(local_file.fileno())
        subprocess.run(
            target.command("push", adb_local_path(target, local_path), path),
            check=True,
            stdout=subprocess.DEVNULL,
        )
        run_adb_shell(target, f"chmod 0600 {path}")
    finally:
        try:
            with open(local_path, "r+b", buffering=0) as local_file:
                local_file.write(b"\0" * len(content))
                os.fsync(local_file.fileno())
        except FileNotFoundError:
            pass
        try:
            os.unlink(local_path)
        except FileNotFoundError:
            pass


def cleanup_remote(target: AdbTarget, paths: Sequence[str]) -> None:
    if not paths:
        return
    run_adb_shell(target, "rm -f -- " + " ".join(paths), check=False)


def check_remote_config(target: AdbTarget, endpoint_binary: str) -> None:
    run_adb_shell(target, f"{shlex.quote(endpoint_binary)} --check-config")


def restart_endpoint(target: AdbTarget) -> None:
    run_adb_shell(target, f"{ENDPOINT_SERVICE} voice-assistant restart")


def provision(args: argparse.Namespace) -> int:
    endpoint = validate_endpoint(args.endpoint)
    satellite_id = validate_satellite_id(args.satellite_id)
    expected_area_id = None
    if args.expected_area_id is not None:
        expected_area_id = validate_area_id(args.expected_area_id)
    credential = read_credential()

    document: dict[str, object] = {
        "schemaVersion": 1,
        "endpoint": endpoint,
        "satelliteId": satellite_id,
    }
    if expected_area_id is not None:
        document["expectedAreaId"] = expected_area_id
    config_bytes = (json.dumps(document, separators=(",", ":")) + "\n").encode()

    target = AdbTarget(args.adb, args.serial)
    token = secrets.token_hex(8)
    config_temp = f"{REMOTE_DIR}/.config.json.{token}.tmp"
    credential_temp = f"{REMOTE_DIR}/.credential.{token}.tmp"
    temporary_paths = (config_temp, credential_temp)

    prepare_remote_directory(target)
    try:
        write_remote_temp(target, config_temp, config_bytes)
        write_remote_temp(target, credential_temp, credential)
        run_adb_shell(
            target,
            f"mv -f {credential_temp} {REMOTE_CREDENTIAL} && "
            f"mv -f {config_temp} {REMOTE_CONFIG} && "
            f"chmod 0600 {REMOTE_CREDENTIAL} {REMOTE_CONFIG}",
        )
    finally:
        cleanup_remote(target, temporary_paths)

    check_remote_config(target, args.endpoint_binary)
    restart_endpoint(target)
    print(f"Provisioned Cortana endpoint {satellite_id}")
    return 0


def rotate_credential(args: argparse.Namespace) -> int:
    credential = read_credential()
    target = AdbTarget(args.adb, args.serial)
    token = secrets.token_hex(8)
    credential_temp = f"{REMOTE_DIR}/.credential.{token}.tmp"

    prepare_remote_directory(target)
    try:
        write_remote_temp(target, credential_temp, credential)
        run_adb_shell(
            target,
            f"mv -f {credential_temp} {REMOTE_CREDENTIAL} && "
            f"chmod 0600 {REMOTE_CREDENTIAL}",
        )
    finally:
        cleanup_remote(target, (credential_temp,))

    check_remote_config(target, args.endpoint_binary)
    restart_endpoint(target)
    print("Rotated Cortana endpoint credential")
    return 0


def status(args: argparse.Namespace) -> int:
    target = AdbTarget(args.adb, args.serial)
    completed = run_adb_shell(
        target,
        f"{shlex.quote(args.endpoint_binary)} --status",
        check=False,
    )
    return completed.returncode


def add_adb_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--adb", default="adb", help="ADB executable")
    parser.add_argument("--serial", help="ADB device serial when multiple exist")
    parser.add_argument(
        "--endpoint-binary",
        default=DEFAULT_ENDPOINT_BINARY,
        help="endpoint binary on the device",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Provision Cortana endpoint files over USB ADB. The credential is "
            "read from a no-echo prompt or standard input; it is never accepted "
            "as an argument."
        )
    )
    commands = parser.add_subparsers(dest="command", required=True)

    provision_parser = commands.add_parser("provision")
    add_adb_options(provision_parser)
    provision_parser.add_argument("--endpoint", required=True)
    provision_parser.add_argument("--satellite-id", required=True)
    provision_parser.add_argument("--expected-area-id")
    provision_parser.set_defaults(handler=provision)

    rotate_parser = commands.add_parser("rotate-credential")
    add_adb_options(rotate_parser)
    rotate_parser.set_defaults(handler=rotate_credential)

    status_parser = commands.add_parser("status")
    add_adb_options(status_parser)
    status_parser.set_defaults(handler=status)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.handler(args)
    except UnicodeEncodeError:
        print("error: credential must contain printable ASCII", file=sys.stderr)
        return 2
    except ProvisionError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except FileNotFoundError as error:
        print(f"error: executable not found: {error.filename}", file=sys.stderr)
        return 127
    except subprocess.CalledProcessError as error:
        print(f"error: ADB command failed with exit code {error.returncode}",
              file=sys.stderr)
        return error.returncode or 1


if __name__ == "__main__":
    raise SystemExit(main())
