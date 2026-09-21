#!/usr/bin/env python3
"""Command-line client for the Arduino Mega AT28C256 programmer."""

from __future__ import annotations

import argparse
import sys
import time
import zlib
from pathlib import Path

import serial
from serial.tools import list_ports

EEPROM_SIZE = 32 * 1024
BAUD = 250_000
PAGE_SIZE = 64


def integer(value: str) -> int:
    """Accept decimal values or prefixed values such as 0x8000."""
    return int(value, 0)


def discover_port() -> str:
    candidates = []
    for port in list_ports.comports():
        name = port.device.lower()
        if port.vid is not None or any(
            marker in name
            for marker in ("usbserial", "usbmodem", "ttyusb", "ttyacm")
        ):
            candidates.append(port.device)

    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise RuntimeError("no USB serial port found; pass --port explicitly")
    raise RuntimeError(
        "more than one USB serial port found; pass --port: "
        + ", ".join(candidates)
    )


class Programmer:
    def __init__(self, port: str):
        self.serial = serial.Serial(
            port=port,
            baudrate=BAUD,
            timeout=1.0,
            write_timeout=10.0,
        )
        # Opening the serial port normally resets an Arduino Mega.
        time.sleep(2.0)
        self.serial.reset_input_buffer()
        self.serial.reset_output_buffer()
        self.command("PING")
        self.wait_for("OK ")

    def close(self) -> None:
        self.serial.close()

    def command(self, text: str) -> None:
        self.serial.write(text.encode("ascii") + b"\n")
        self.serial.flush()

    def line(self) -> str:
        raw = self.serial.readline()
        if not raw:
            raise TimeoutError("programmer did not respond")
        return raw.decode("ascii", errors="replace").strip()

    def wait_for(self, prefix: str) -> str:
        while True:
            response = self.line()
            if response.startswith("ERR "):
                raise RuntimeError(response)
            if response.startswith(prefix):
                return response

    def info(self) -> str:
        self.command("INFO")
        return self.wait_for("OK ")

    def write(self, start: int, data: bytes, trust_cache: bool) -> str:
        crc = zlib.crc32(data) & 0xFFFFFFFF
        option = " TRUST" if trust_cache else ""
        self.command(f"WRITE {start} {len(data)} {crc:08X}{option}")
        while True:
            response = self.line()
            if response.startswith("ERR "):
                raise RuntimeError(response)
            if response.startswith("OK "):
                # A trusted cache hit completes before payload transfer.
                return response
            if response == "READY":
                break

        sent = 0
        while sent < len(data):
            address = start + sent
            count = min(PAGE_SIZE - (address % PAGE_SIZE), len(data) - sent)
            chunk = data[sent : sent + count]
            page_crc = zlib.crc32(chunk) & 0xFFFFFFFF
            self.command(f"PAGE {address} {len(chunk)} {page_crc:08X}")
            self.wait_for("SEND")
            self.serial.write(chunk)
            self.serial.flush()
            sent += len(chunk)
            acknowledgement = self.wait_for("ACK ")
            print(
                f"\r{acknowledgement} ({sent * 100 // len(data)}%)",
                end="",
                flush=True,
            )
        print()

        while True:
            response = self.line()
            if response.startswith("ERR "):
                raise RuntimeError(response)
            elif response.startswith("OK "):
                return response

    def read(self, start: int, length: int) -> bytes:
        self.command(f"READ {start} {length}")
        header = self.wait_for("DATA ")
        fields = header.split()
        if len(fields) != 3 or int(fields[1]) != length:
            raise RuntimeError(f"invalid DATA header: {header}")

        expected_crc = int(fields[2], 16)
        data = self.serial.read(length)
        while len(data) < length:
            chunk = self.serial.read(length - len(data))
            if not chunk:
                raise TimeoutError("timed out while receiving EEPROM data")
            data += chunk

        # Firmware sends one line break after the raw bytes, then OK.
        self.serial.readline()
        self.wait_for("OK")
        actual_crc = zlib.crc32(data) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise RuntimeError(
                f"read CRC mismatch: expected {expected_crc:08X}, got {actual_crc:08X}"
            )
        return data


def checked_range(start: int, length: int) -> None:
    if start < 0 or length <= 0 or start + length > EEPROM_SIZE:
        raise ValueError(
            f"range must fit in 0x0000..0x{EEPROM_SIZE - 1:04X}"
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Fast incremental AT28C256 programmer client"
    )
    parser.add_argument("--port", help="serial device; auto-detected if omitted")
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("info", help="show programmer and remembered upload info")

    write = commands.add_parser("write", help="incrementally program a binary file")
    write.add_argument("file", type=Path)
    write.add_argument("--offset", type=integer, default=0)
    write.add_argument(
        "--fill",
        type=integer,
        metavar="BYTE",
        help="pad from the file end to 32 KiB (for example --fill 0xFF)",
    )
    write.add_argument(
        "--trust-cache",
        action="store_true",
        help="skip the EEPROM scan when the Mega remembers the same upload",
    )

    read = commands.add_parser("read", help="read a range into a file")
    read.add_argument("file", type=Path)
    read.add_argument("--offset", type=integer, default=0)
    read.add_argument("--length", type=integer, required=True)

    dump = commands.add_parser("dump", help="read all 32 KiB into a file")
    dump.add_argument("file", type=Path)

    verify = commands.add_parser("verify", help="compare a binary file with EEPROM")
    verify.add_argument("file", type=Path)
    verify.add_argument("--offset", type=integer, default=0)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        port = args.port or discover_port()
        programmer = Programmer(port)
        try:
            if args.command == "info":
                print(programmer.info())

            elif args.command == "write":
                data = args.file.read_bytes()
                if not data:
                    raise ValueError("input file is empty")
                if args.fill is not None:
                    if not 0 <= args.fill <= 0xFF:
                        raise ValueError("--fill must be a byte from 0x00 to 0xFF")
                    data += bytes([args.fill]) * (EEPROM_SIZE - args.offset - len(data))
                checked_range(args.offset, len(data))
                print(programmer.write(args.offset, data, args.trust_cache))

            elif args.command == "read":
                checked_range(args.offset, args.length)
                data = programmer.read(args.offset, args.length)
                args.file.write_bytes(data)
                print(f"Read {len(data)} bytes into {args.file}")

            elif args.command == "dump":
                data = programmer.read(0, EEPROM_SIZE)
                args.file.write_bytes(data)
                print(f"Read {len(data)} bytes into {args.file}")

            elif args.command == "verify":
                expected = args.file.read_bytes()
                checked_range(args.offset, len(expected))
                actual = programmer.read(args.offset, len(expected))
                if actual != expected:
                    first = next(
                        i for i, (left, right) in enumerate(zip(expected, actual))
                        if left != right
                    )
                    address = args.offset + first
                    raise RuntimeError(
                        f"verify failed at 0x{address:04X}: "
                        f"file=0x{expected[first]:02X}, EEPROM=0x{actual[first]:02X}"
                    )
                print(f"OK: {len(expected)} bytes match at 0x{args.offset:04X}")
        finally:
            programmer.close()
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
