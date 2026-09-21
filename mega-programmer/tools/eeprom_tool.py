#!/usr/bin/env python3
"""CLI and Tk GUI for the in-circuit AT28C256 Mega programmer."""

from __future__ import annotations

import argparse
import queue
import sys
import threading
import time
import zlib
from pathlib import Path
from typing import Callable

try:
    import serial
    from serial.tools import list_ports
except ImportError as error:
    raise SystemExit(
        "pyserial is required. Install it with: "
        "python3 -m pip install -r requirements.txt"
    ) from error


EEPROM_SIZE = 32 * 1024
PAGE_SIZE = 64
BAUD = 500_000
Progress = Callable[[int, int, str], None]


def integer(value: str) -> int:
    """Accept decimal or prefixed values such as 0x8000."""
    return int(value, 0)


def available_ports() -> list[str]:
    return [port.device for port in list_ports.comports()]


def discover_port() -> str:
    candidates: list[str] = []
    for port in list_ports.comports():
        name = port.device.lower()
        if port.vid is not None or any(
            marker in name
            for marker in ("usbmodem", "usbserial", "ttyacm", "ttyusb")
        ):
            candidates.append(port.device)
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise RuntimeError("no USB serial port found; pass --port")
    raise RuntimeError(
        "multiple USB serial ports found; pass --port: "
        + ", ".join(candidates)
    )


def checked_range(start: int, length: int) -> None:
    if start < 0 or length <= 0 or start + length > EEPROM_SIZE:
        raise ValueError(
            f"range must fit in 0x0000..0x{EEPROM_SIZE - 1:04X}"
        )


class Programmer:
    """Synchronous protocol client. Use only one reader thread at a time."""

    def __init__(self, port: str, log: Callable[[str], None] | None = None):
        self.port = port
        self.log = log
        self.mode = "unknown"
        self.serial = serial.Serial(
            port=port,
            baudrate=BAUD,
            timeout=0.25,
            write_timeout=5.0,
        )

        # Opening a Mega's USB serial port normally toggles DTR and resets it.
        time.sleep(1.8)
        self.serial.reset_input_buffer()
        self.serial.reset_output_buffer()
        response = self.command("PING", "@OK ", timeout=3.0)
        self.mode = self._field(response, "mode", "debug")

    def close(self) -> None:
        if self.serial.is_open:
            self.serial.close()

    def __enter__(self) -> "Programmer":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    @staticmethod
    def _field(line: str, key: str, default: str = "") -> str:
        prefix = key + "="
        for field in line.split():
            if field.startswith(prefix):
                return field[len(prefix) :]
        return default

    def _send_line(self, line: str) -> None:
        if self.log:
            self.log("> " + line)
        self.serial.write(line.encode("ascii") + b"\n")
        self.serial.flush()

    def _read_line(self, deadline: float) -> str:
        while time.monotonic() < deadline:
            raw = self.serial.readline()
            if not raw:
                continue
            line = raw.decode("ascii", errors="replace").strip()
            if line:
                if self.log and line.startswith("@"):
                    self.log("< " + line)
                return line
        raise TimeoutError("programmer did not respond")

    def _wait_for(self, prefix: str, timeout: float = 3.0) -> str:
        deadline = time.monotonic() + timeout
        while True:
            line = self._read_line(deadline)
            if line.startswith("@ERR "):
                raise RuntimeError(line[5:])
            if line.startswith(prefix):
                return line
            # Non-@ lines are debugger bus samples left in the serial queue.

    def command(self, text: str, prefix: str = "@OK ", timeout: float = 3.0) -> str:
        self._send_line(text)
        return self._wait_for(prefix, timeout)

    def info(self) -> str:
        response = self.command("INFO")
        self.mode = self._field(response, "mode", self.mode)
        return response

    def set_mode(self, mode: str) -> str:
        normalized = mode.lower()
        if normalized not in {"debug", "program"}:
            raise ValueError("mode must be debug or program")
        if normalized == "program":
            # Discard queued debugger lines before requesting bus ownership.
            self.serial.reset_input_buffer()
        response = self.command(f"MODE {normalized.upper()}", timeout=5.0)
        self.mode = normalized
        return response

    def reset_cpu(self) -> str:
        response = self.command("RESET")
        self.mode = "debug"
        return response

    def set_trace(self, enabled: bool) -> str:
        if self.mode != "debug":
            raise RuntimeError("bus trace is available only in debug mode")
        return self.command("TRACE ON" if enabled else "TRACE OFF")

    def write(
        self,
        start: int,
        data: bytes,
        progress: Progress | None = None,
    ) -> str:
        checked_range(start, len(data))
        if self.mode != "program":
            self.set_mode("program")

        image_crc = zlib.crc32(data) & 0xFFFFFFFF
        self._send_line(f"WRITE {start} {len(data)} {image_crc:08X}")
        self._wait_for("@READY", 5.0)

        sent = 0
        while sent < len(data):
            address = start + sent
            count = min(PAGE_SIZE - (address % PAGE_SIZE), len(data) - sent)
            block = data[sent : sent + count]
            block_crc = zlib.crc32(block) & 0xFFFFFFFF
            self._send_line(f"PAGE {address} {count} {block_crc:08X}")
            self._wait_for("@SEND", 3.0)
            self.serial.write(block)
            self.serial.flush()
            acknowledgement = self._wait_for("@ACK ", 25.0)
            sent += count
            if progress:
                progress(sent, len(data), acknowledgement)

        return self._wait_for("@OK ", 5.0)

    def read(
        self,
        start: int,
        length: int,
        progress: Progress | None = None,
    ) -> bytes:
        checked_range(start, length)
        if self.mode != "program":
            self.set_mode("program")

        self._send_line(f"READ {start} {length}")
        header = self._wait_for("@DATA ", 5.0)
        fields = header.split()
        if len(fields) != 2 or int(fields[1]) != length:
            raise RuntimeError(f"invalid read header: {header}")

        data = bytearray()
        deadline = time.monotonic() + max(5.0, length / 20_000)
        while len(data) < length:
            block = self.serial.read(min(4096, length - len(data)))
            if block:
                data.extend(block)
                if progress:
                    progress(len(data), length, "reading")
                continue
            if time.monotonic() >= deadline:
                raise TimeoutError(
                    f"read stopped after {len(data)} of {length} bytes"
                )

        response = self._wait_for("@OK ", 3.0)
        expected_crc = int(self._field(response, "crc32"), 16)
        actual_crc = zlib.crc32(data) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise RuntimeError(
                f"read CRC mismatch: device={expected_crc:08X}, "
                f"host={actual_crc:08X}"
            )
        return bytes(data)

    def verify(
        self,
        start: int,
        expected: bytes,
        progress: Progress | None = None,
    ) -> str:
        actual = self.read(start, len(expected), progress)
        if actual != expected:
            first = next(
                index
                for index, (wanted, found) in enumerate(zip(expected, actual))
                if wanted != found
            )
            address = start + first
            raise RuntimeError(
                f"verify failed at EEPROM 0x{address:04X}: "
                f"file=0x{expected[first]:02X}, EEPROM=0x{actual[first]:02X}"
            )
        return f"OK: {len(expected)} bytes match at EEPROM 0x{start:04X}"

    def debug_line(self) -> str | None:
        raw = self.serial.readline()
        if not raw:
            return None
        return raw.decode("ascii", errors="replace").rstrip()


def launch_gui(initial_port: str | None = None) -> int:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    class EepromApp:
        def __init__(self, root: tk.Tk):
            self.root = root
            self.root.title("6502 Mega - Debug & EEPROM")
            self.root.geometry("920x680")
            self.root.minsize(760, 560)
            self.programmer: Programmer | None = None
            self.monitor_stop = threading.Event()
            self.monitor_thread: threading.Thread | None = None
            # A bounded queue is essential: a fast CPU can produce trace text
            # faster than Tk can render it. Old debug lines are expendable.
            self.messages: queue.Queue[tuple[str, object]] = queue.Queue(
                maxsize=512
            )
            self.busy = False
            self.trace_enabled = False
            self.console_line_count = 0

            self.port_var = tk.StringVar(value=initial_port or "")
            self.mode_var = tk.StringVar(value="Disconnected")
            self.status_var = tk.StringVar(value="Choose a serial port and connect")
            self.offset_var = tk.StringVar(value="0x0000")
            self.length_var = tk.StringVar(value="0x8000")
            self.file_var = tk.StringVar()
            self.fill_var = tk.BooleanVar(value=False)
            self.progress_var = tk.DoubleVar(value=0)

            self._style()
            self._build()
            self.refresh_ports()
            self.root.after(50, self._drain_messages)
            self.root.protocol("WM_DELETE_WINDOW", self.close)

        def _style(self) -> None:
            style = ttk.Style()
            if "clam" in style.theme_names():
                style.theme_use("clam")
            style.configure("Title.TLabel", font=("TkDefaultFont", 18, "bold"))
            style.configure("Mode.TButton", font=("TkDefaultFont", 12, "bold"), padding=12)
            style.configure("Action.TButton", padding=(12, 8))

        def _build(self) -> None:
            outer = ttk.Frame(self.root, padding=18)
            outer.pack(fill="both", expand=True)

            ttk.Label(outer, text="6502 Mega Controller", style="Title.TLabel").pack(
                anchor="w"
            )
            ttk.Label(
                outer,
                text="Fast in-circuit AT28C256 programming and live bus debug",
            ).pack(anchor="w", pady=(2, 14))

            connection = ttk.Frame(outer)
            connection.pack(fill="x")
            ttk.Label(connection, text="Serial port").pack(side="left")
            self.port_box = ttk.Combobox(
                connection, textvariable=self.port_var, width=34
            )
            self.port_box.pack(side="left", padx=(8, 6))
            ttk.Button(connection, text="Refresh", command=self.refresh_ports).pack(
                side="left"
            )
            self.connect_button = ttk.Button(
                connection, text="Connect", command=self.toggle_connection
            )
            self.connect_button.pack(side="left", padx=6)
            ttk.Label(connection, textvariable=self.mode_var).pack(side="right")

            modes = ttk.LabelFrame(outer, text="Mode", padding=12)
            modes.pack(fill="x", pady=14)
            ttk.Button(
                modes,
                text="Debug / Run",
                style="Mode.TButton",
                command=lambda: self.change_mode("debug"),
            ).pack(side="left", fill="x", expand=True, padx=(0, 6))
            ttk.Button(
                modes,
                text="EEPROM Programmer",
                style="Mode.TButton",
                command=lambda: self.change_mode("program"),
            ).pack(side="left", fill="x", expand=True, padx=(6, 0))
            self.trace_button = ttk.Button(
                modes,
                text="Start Bus Trace",
                style="Mode.TButton",
                command=self.toggle_trace,
            )
            self.trace_button.pack(side="left", fill="x", expand=True, padx=(12, 0))

            programmer = ttk.LabelFrame(outer, text="EEPROM image", padding=12)
            programmer.pack(fill="x")
            file_row = ttk.Frame(programmer)
            file_row.pack(fill="x")
            ttk.Entry(file_row, textvariable=self.file_var).pack(
                side="left", fill="x", expand=True
            )
            ttk.Button(file_row, text="Browse…", command=self.choose_file).pack(
                side="left", padx=(8, 0)
            )

            options = ttk.Frame(programmer)
            options.pack(fill="x", pady=(10, 8))
            ttk.Label(options, text="EEPROM offset").pack(side="left")
            ttk.Entry(options, textvariable=self.offset_var, width=10).pack(
                side="left", padx=(6, 18)
            )
            ttk.Label(options, text="Read length").pack(side="left")
            ttk.Entry(options, textvariable=self.length_var, width=10).pack(
                side="left", padx=(6, 18)
            )
            ttk.Checkbutton(
                options,
                text="Fill remaining EEPROM with 0xFF",
                variable=self.fill_var,
            ).pack(side="left")

            actions = ttk.Frame(programmer)
            actions.pack(fill="x")
            ttk.Button(
                actions,
                text="Program",
                style="Action.TButton",
                command=self.program_file,
            ).pack(side="left")
            ttk.Button(
                actions,
                text="Verify",
                style="Action.TButton",
                command=self.verify_file,
            ).pack(side="left", padx=6)
            ttk.Button(
                actions,
                text="Read Range…",
                style="Action.TButton",
                command=self.read_range,
            ).pack(side="left")
            ttk.Button(
                actions,
                text="Dump 32 KiB…",
                style="Action.TButton",
                command=self.dump,
            ).pack(side="left", padx=6)
            ttk.Button(
                actions,
                text="Reset CPU",
                style="Action.TButton",
                command=self.reset_cpu,
            ).pack(side="right")

            ttk.Progressbar(
                outer, variable=self.progress_var, maximum=100
            ).pack(fill="x", pady=(12, 4))
            ttk.Label(outer, textvariable=self.status_var).pack(anchor="w")

            console_frame = ttk.LabelFrame(
                outer, text="Debugger / activity", padding=6
            )
            console_frame.pack(fill="both", expand=True, pady=(12, 0))
            self.console = tk.Text(
                console_frame,
                height=13,
                wrap="none",
                bg="#111827",
                fg="#d1fae5",
                insertbackground="white",
                font=("Menlo", 10),
            )
            scrollbar = ttk.Scrollbar(
                console_frame, orient="vertical", command=self.console.yview
            )
            self.console.configure(yscrollcommand=scrollbar.set)
            self.console.pack(side="left", fill="both", expand=True)
            scrollbar.pack(side="right", fill="y")

        def log(self, line: str) -> None:
            self._post("log", line)

        def _post(self, kind: str, payload: object, drop_if_full: bool = False) -> None:
            try:
                self.messages.put_nowait((kind, payload))
            except queue.Full:
                if drop_if_full:
                    return
                # Preserve control/status events by evicting one stale item.
                try:
                    self.messages.get_nowait()
                except queue.Empty:
                    pass
                try:
                    self.messages.put_nowait((kind, payload))
                except queue.Full:
                    pass

        def refresh_ports(self) -> None:
            ports = available_ports()
            self.port_box["values"] = ports
            if not self.port_var.get() and ports:
                self.port_var.set(ports[0])

        def toggle_connection(self) -> None:
            if self.programmer:
                self.disconnect()
                return
            port = self.port_var.get().strip()
            if not port:
                messagebox.showerror("Serial port", "Choose a serial port first.")
                return

            def connect() -> tuple[Programmer, str]:
                device = Programmer(port, self.log)
                return device, device.info()

            def connected(result: object) -> None:
                device, info = result  # type: ignore[misc]
                self.programmer = device
                self.connect_button.configure(text="Disconnect")
                self.mode_var.set("Mode: " + device.mode.upper())
                self.status_var.set(info)
                self.trace_enabled = False
                self.trace_button.configure(text="Start Bus Trace")

            self.run_job("Connecting…", connect, connected)

        def disconnect(self) -> None:
            self.stop_monitor()
            if self.programmer:
                self.programmer.close()
                self.programmer = None
            self.connect_button.configure(text="Connect")
            self.mode_var.set("Disconnected")
            self.status_var.set("Disconnected")
            self.trace_enabled = False
            self.trace_button.configure(text="Start Bus Trace")

        def require_device(self) -> Programmer | None:
            if not self.programmer:
                messagebox.showerror("Not connected", "Connect to the Mega first.")
                return None
            return self.programmer

        def change_mode(self, mode: str) -> None:
            device = self.require_device()
            if not device:
                return
            self.stop_monitor()

            def changed(result: object) -> None:
                self.mode_var.set("Mode: " + mode.upper())
                self.status_var.set(str(result))
                self.trace_enabled = False
                self.trace_button.configure(text="Start Bus Trace")

            self.run_job(
                f"Switching to {mode}…",
                lambda: device.set_mode(mode),
                changed,
            )

        def reset_cpu(self) -> None:
            device = self.require_device()
            if not device:
                return
            self.stop_monitor()

            def reset_done(result: object) -> None:
                self.mode_var.set("Mode: DEBUG")
                self.status_var.set(str(result))
                self.trace_enabled = False
                self.trace_button.configure(text="Start Bus Trace")

            self.run_job("Resetting 6502…", device.reset_cpu, reset_done)

        def toggle_trace(self) -> None:
            device = self.require_device()
            if not device:
                return
            if device.mode != "debug":
                messagebox.showerror(
                    "Bus trace", "Switch to Debug / Run mode first."
                )
                return

            enabling = not self.trace_enabled
            self.stop_monitor()

            def changed(result: object) -> None:
                self.trace_enabled = enabling
                self.trace_button.configure(
                    text="Stop Bus Trace" if enabling else "Start Bus Trace"
                )
                self.status_var.set(str(result))
                if enabling:
                    self.start_monitor()

            self.run_job(
                "Starting bounded bus trace…" if enabling else "Stopping bus trace…",
                lambda: device.set_trace(enabling),
                changed,
            )

        def choose_file(self) -> None:
            filename = filedialog.askopenfilename(
                title="Choose ROM binary",
                filetypes=(("Binary images", "*.bin *.rom"), ("All files", "*")),
            )
            if filename:
                self.file_var.set(filename)

        def input_image(self) -> tuple[int, bytes]:
            path = Path(self.file_var.get()).expanduser()
            if not path.is_file():
                raise ValueError("choose an existing binary image")
            start = integer(self.offset_var.get())
            data = path.read_bytes()
            if not data:
                raise ValueError("the selected image is empty")
            if self.fill_var.get():
                checked_range(start, len(data))
                data += bytes([0xFF]) * (EEPROM_SIZE - start - len(data))
            checked_range(start, len(data))
            return start, data

        def progress(self, done: int, total: int, detail: str) -> None:
            self._post("progress", (done, total, detail))

        def program_file(self) -> None:
            device = self.require_device()
            if not device:
                return
            try:
                start, data = self.input_image()
            except (OSError, ValueError) as error:
                messagebox.showerror("Image", str(error))
                return
            self.stop_monitor()

            def programmed(result: object) -> None:
                self.mode_var.set("Mode: PROGRAM")
                self.status_var.set(str(result))
                self.progress_var.set(100)

            self.run_job(
                f"Programming {len(data)} bytes…",
                lambda: device.write(start, data, self.progress),
                programmed,
            )

        def verify_file(self) -> None:
            device = self.require_device()
            if not device:
                return
            try:
                start, data = self.input_image()
            except (OSError, ValueError) as error:
                messagebox.showerror("Image", str(error))
                return
            self.stop_monitor()
            self.run_job(
                f"Verifying {len(data)} bytes…",
                lambda: device.verify(start, data, self.progress),
                lambda result: self._operation_done(result),
            )

        def read_range(self) -> None:
            device = self.require_device()
            if not device:
                return
            try:
                start = integer(self.offset_var.get())
                length = integer(self.length_var.get())
                checked_range(start, length)
            except ValueError as error:
                messagebox.showerror("Range", str(error))
                return
            filename = filedialog.asksaveasfilename(
                title="Save EEPROM range",
                defaultextension=".bin",
                filetypes=(("Binary image", "*.bin"), ("All files", "*")),
            )
            if not filename:
                return
            self.stop_monitor()

            def read_and_save() -> str:
                data = device.read(start, length, self.progress)
                Path(filename).write_bytes(data)
                return f"Saved {len(data)} bytes to {filename}"

            self.run_job("Reading EEPROM…", read_and_save, self._operation_done)

        def dump(self) -> None:
            device = self.require_device()
            if not device:
                return
            filename = filedialog.asksaveasfilename(
                title="Save complete EEPROM",
                defaultextension=".bin",
                filetypes=(("Binary image", "*.bin"), ("All files", "*")),
            )
            if not filename:
                return
            self.stop_monitor()

            def dump_and_save() -> str:
                data = device.read(0, EEPROM_SIZE, self.progress)
                Path(filename).write_bytes(data)
                return f"Saved complete 32 KiB EEPROM to {filename}"

            self.run_job("Dumping EEPROM…", dump_and_save, self._operation_done)

        def _operation_done(self, result: object) -> None:
            self.mode_var.set("Mode: PROGRAM")
            self.status_var.set(str(result))
            self.progress_var.set(100)

        def run_job(
            self,
            label: str,
            action: Callable[[], object],
            success: Callable[[object], None],
        ) -> None:
            if self.busy:
                messagebox.showinfo("Busy", "Wait for the current operation.")
                return
            self.busy = True
            self.status_var.set(label)
            self.progress_var.set(0)

            def worker() -> None:
                try:
                    result = action()
                except Exception as error:  # surfaced in the GUI
                    self._post("error", error)
                else:
                    self._post("success", (success, result))
                finally:
                    self._post("idle", None)

            threading.Thread(target=worker, daemon=True).start()

        def start_monitor(self) -> None:
            if not self.programmer or self.monitor_thread:
                return
            self.monitor_stop.clear()

            def monitor() -> None:
                while not self.monitor_stop.is_set() and self.programmer:
                    line = self.programmer.debug_line()
                    if line:
                        self._post("debug", line, drop_if_full=True)

            self.monitor_thread = threading.Thread(target=monitor, daemon=True)
            self.monitor_thread.start()

        def stop_monitor(self) -> None:
            self.monitor_stop.set()
            if self.monitor_thread:
                self.monitor_thread.join(timeout=0.6)
                self.monitor_thread = None

        def _drain_messages(self) -> None:
            # Never drain until empty: a continuous producer could otherwise
            # monopolize Tk's event loop forever. Process a fixed batch.
            for _ in range(200):
                try:
                    kind, payload = self.messages.get_nowait()
                except queue.Empty:
                    break
                if kind in {"log", "debug"}:
                    self.console.insert("end", str(payload) + "\n")
                    self.console_line_count += 1
                    if self.console_line_count > 2000:
                        self.console.delete("1.0", "501.0")
                        self.console_line_count -= 500
                    self.console.see("end")
                elif kind == "progress":
                    done, total, detail = payload  # type: ignore[misc]
                    self.progress_var.set(done * 100 / total)
                    self.status_var.set(
                        f"{done}/{total} bytes ({done * 100 // total}%) - {detail}"
                    )
                elif kind == "success":
                    callback, result = payload  # type: ignore[misc]
                    callback(result)
                elif kind == "error":
                    self.status_var.set("Operation failed")
                    messagebox.showerror("EEPROM programmer", str(payload))
                elif kind == "idle":
                    self.busy = False
            self.root.after(50, self._drain_messages)

        def close(self) -> None:
            self.disconnect()
            self.root.destroy()

    root = tk.Tk()
    EepromApp(root)
    root.mainloop()
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Fast Arduino Mega AT28C256 programmer and debugger"
    )
    parser.add_argument("--port", help="serial device; auto-detected if omitted")
    commands = parser.add_subparsers(dest="command")

    commands.add_parser("gui", help="open the graphical interface (default)")
    commands.add_parser("ports", help="list serial ports")
    commands.add_parser("info", help="show firmware and EEPROM information")

    mode = commands.add_parser("mode", help="switch bus ownership mode")
    mode.add_argument("mode", choices=("debug", "program"))

    trace = commands.add_parser("trace", help="start or stop bounded bus trace")
    trace.add_argument("state", choices=("on", "off"))

    commands.add_parser("reset", help="restart the 6502 in debug mode")

    write = commands.add_parser("write", help="program a binary image")
    write.add_argument("file", type=Path)
    write.add_argument("--offset", type=integer, default=0)
    write.add_argument(
        "--fill",
        type=integer,
        metavar="BYTE",
        help="pad from the file end to 32 KiB, e.g. --fill 0xFF",
    )

    read = commands.add_parser("read", help="read an EEPROM range")
    read.add_argument("file", type=Path)
    read.add_argument("--offset", type=integer, default=0)
    read.add_argument("--length", type=integer, required=True)

    dump = commands.add_parser("dump", help="read the complete 32 KiB EEPROM")
    dump.add_argument("file", type=Path)

    verify = commands.add_parser("verify", help="compare EEPROM with an image")
    verify.add_argument("file", type=Path)
    verify.add_argument("--offset", type=integer, default=0)
    return parser


def terminal_progress(done: int, total: int, detail: str) -> None:
    print(
        f"\r{done:5d}/{total:5d} bytes ({done * 100 // total:3d}%) {detail[:50]:50}",
        end="",
        flush=True,
    )
    if done == total:
        print()


def main() -> int:
    args = build_parser().parse_args()
    if args.command in {None, "gui"}:
        return launch_gui(args.port)
    if args.command == "ports":
        ports = available_ports()
        print("\n".join(ports) if ports else "No serial ports found")
        return 0

    try:
        port = args.port or discover_port()
        with Programmer(port) as device:
            if args.command == "info":
                print(device.info())
            elif args.command == "mode":
                print(device.set_mode(args.mode))
            elif args.command == "trace":
                if device.mode != "debug":
                    device.set_mode("debug")
                print(device.set_trace(args.state == "on"))
            elif args.command == "reset":
                if device.mode != "debug":
                    device.set_mode("debug")
                print(device.reset_cpu())
            elif args.command == "write":
                data = args.file.read_bytes()
                if not data:
                    raise ValueError("input file is empty")
                if args.fill is not None:
                    if not 0 <= args.fill <= 0xFF:
                        raise ValueError("--fill must be between 0x00 and 0xFF")
                    checked_range(args.offset, len(data))
                    data += bytes([args.fill]) * (
                        EEPROM_SIZE - args.offset - len(data)
                    )
                print(device.write(args.offset, data, terminal_progress))
            elif args.command == "read":
                data = device.read(args.offset, args.length, terminal_progress)
                args.file.write_bytes(data)
                print(f"Saved {len(data)} bytes to {args.file}")
            elif args.command == "dump":
                data = device.read(0, EEPROM_SIZE, terminal_progress)
                args.file.write_bytes(data)
                print(f"Saved complete 32 KiB EEPROM to {args.file}")
            elif args.command == "verify":
                data = args.file.read_bytes()
                print(device.verify(args.offset, data, terminal_progress))
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
