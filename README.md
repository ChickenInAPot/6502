# 6502 breadboard computer tools

EEPROM programming and bus-debugging tools for my 6502 breadboard computer. The main hardware is an Arduino Mega 2560, a W65C02, and an AT28C256 EEPROM.

**Main development period: July 2026.** This builds on earlier experiments already in the repository history. The July work adds the Mega programmer, a Python interface, and a small counting ROM. The documentation was organized for GitHub in September 2026.

## What is here

| Folder | Purpose |
| --- | --- |
| [`mega-programmer/`](mega-programmer/) | Mega firmware, Python GUI/CLI, and standalone or in-circuit EEPROM programming |
| [`mega-programmer/rom/`](mega-programmer/rom/) | W65C02 counting program and ca65/ld65 build files |
| [`shift-register-programmer/`](shift-register-programmer/) | An alternative programmer using two 74HC595 address shift registers |

The two programmers use different wiring. Start with the Mega's **standalone** build and its [wiring table](mega-programmer/STANDALONE_WIRING.md). Only use the in-circuit build after reading the [bus handoff notes](mega-programmer/README.md).

## How it works

```text
Python CLI / Tk GUI -- USB serial --> Arduino Mega -- address/data bus --> AT28C256
                                          |
                                 optional 6502 bus trace
```

The programmer compares the ROM image against the chip, skips unchanged bytes, writes changed pages, and reads them back to verify the result. Transfers include CRC-32 checks. The in-circuit mode holds the CPU in reset and releases its bus before the Mega takes control.

The bus trace is sampled and rate-limited. It is useful for inspection, but it is not a lossless logic analyzer.

## Build the firmware

Install Python 3 and [PlatformIO](https://platformio.org/). From the repository root:

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install platformio pyserial
pio run -d mega-programmer -e standalone_eeprom
```

With the Mega connected and wired for standalone programming:

```sh
pio run -d mega-programmer -e standalone_eeprom --target upload
```

The in-circuit alternative is `-e incircuit_ben_eater`. The shift-register version builds with `pio run -d shift-register-programmer`.

## Use the host tool

```sh
python mega-programmer/tools/eeprom_tool.py ports
python mega-programmer/tools/eeprom_tool.py --port /dev/cu.usbmodemXXXX info
python mega-programmer/tools/eeprom_tool.py --port /dev/cu.usbmodemXXXX write rom.bin
python mega-programmer/tools/eeprom_tool.py --port /dev/cu.usbmodemXXXX verify rom.bin
```

Replace the serial-port placeholder with the port shown on your machine. Running `python mega-programmer/tools/eeprom_tool.py` without a command opens the Tk GUI; that requires a Python installation with Tk support. Close any serial monitor before connecting.

## Build the counting ROM

Install the [cc65 toolchain](https://cc65.github.io/) (`brew install cc65` on macOS), then run:

```sh
make -C mega-programmer/rom
```

This produces `mega-programmer/rom/build/counting.bin`, a 32 KiB image with its reset vector at `$8000`. See the [ROM notes](mega-programmer/rom/README.md) for its memory map and programming commands.

## Wiring and limits

- Use the wiring for the selected firmware environment. Standalone and in-circuit mode do not share identical control connections.
- The in-circuit handoff relies on the W65C02S `BE` input. Do not assume the same arrangement works with every 6502 variant.
- Keep a common ground and use the specified pull-ups and bypass capacitors. Disconnect power before rewiring or moving the EEPROM.
- Page-write speed estimates in the detailed notes are calculations, not measured benchmark results.

[Validation notes](docs/validation.md) record what was checked while preparing this repository. No upload or EEPROM write is part of those checks.

## Background

The breadboard computer follows the [Ben Eater 6502 series](https://eater.net/6502). This repository contains my project tooling, rather than a claim that I designed the underlying computer architecture. See [project history and references](docs/project-notes.md) for the older experiments and reference material.
