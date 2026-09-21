# Mega debugger and AT28C256 programmer

This project keeps the original Arduino Mega bus debugger and adds an
in-circuit AT28C256 programming mode. The same Python application provides both
a command-line interface and a Tk desktop GUI.

## Page programming

The AT28C256 datasheet specifies a 64-byte page register, a 150 us maximum
inter-byte load window, a 100 ns minimum `/WE` pulse, and D7 completion polling.
The firmware uses all four:

- direct AVR port reads/writes instead of `digitalRead()`/`digitalWrite()`;
- up to 64 changed bytes per EEPROM programming cycle;
- comparison on the Mega, so matching bytes and pages are never rewritten;
- D7 data polling with D6 toggle-bit fallback instead of fixed 10 ms sleeps;
- immediate read-back verification of every changed byte;
- 500000-baud page-framed transfers with CRC-32;
- automatic retry using the AT28C256 AA/55/A0 sequence if software data
  protection is already enabled.

A completely different 32 KiB image needs at most 512 internal page cycles.
This is a timing estimate, not a measured benchmark. The standard AT28C256 specifies 10 ms maximum per page, so its intrinsic
worst-case programming time is about 5.12 seconds, plus roughly 0.7 seconds of
serial transfer and comparison/verification. Small rebuilds are much faster
because unchanged pages are skipped.

## Existing bus wiring

The direct-port mapping matches the original debugger:

| Bus signal | Arduino Mega pins |
|---|---|
| A15 through A0 | 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52 |
| D7 through D0 | 39, 41, 43, 45, 47, 49, 51, 53 |
| 6502 clock | 2 |
| 6502 RWB (debug input) | 3 |

Add these mode and EEPROM controls:

| Signal | Mega pin | Required default |
|---|---:|---|
| W65C02S `/RESET` | 4 | external 10 kOhm pull-up to +5 V |
| W65C02S `BE` | 5 | external 10 kOhm pull-up to +5 V |
| AT28C256 `/CE` | no Mega connection | existing A15 inverter output |
| AT28C256 `/OE` | 7 | external 10 kOhm pull-down to GND |
| AT28C256 `/WE` | 8 | external 10 kOhm pull-up to +5 V |
| Mega pin 9 | no connection | unused |

The programmer uses logical EEPROM addresses `0x0000..0x7FFF` while forcing
system A15 high. Therefore EEPROM offset `0x0000` appears at CPU address
`$8000`, and offset `0x7FFF` appears at `$FFFF`.

## Important no-buffer hardware rule

`BE` low makes the W65C02S address, data and RWB outputs high-impedance. That
makes direct sharing of the address/data bus possible, provided the firmware's
handoff sequence is followed.

`BE` does **not** disable your address decoder, inverter, NAND gates, VIA, RAM,
or other glue-logic outputs. The Ben Eater A15 inverter therefore remains the
only driver of EEPROM `/CE`; leave Mega D6 disconnected. The programmer forces
A15 high, so the inverter holds `/CE` low during EEPROM access.

The original permanent `/OE`-to-GND connection must be removed because writes
require `/OE` high. Connect AT28C256 `/OE` pin 22 directly to Mega D7 and add a
10 kOhm pull-down from that node to GND. In RUN, the Mega releases D7 and the
pull-down enables EEPROM reads. In PROGRAM, the Mega drives D7 high to disable
the EEPROM outputs, or low for reads. Mega D9 is unused.

Remove the EEPROM `/WE` direct +5 V connection. Connect `/WE` to Mega D8 and
add a 10 kOhm pull-up to +5 V; it remains safely high in RUN and the Mega pulses
it low only in PROGRAM.

Keep the Mega and the complete 6502 computer on the same regulated 5 V supply
with a shared ground. Add a 100 nF bypass capacitor at the EEPROM. Optional
220-330 ohm series resistors in the Mega bus/control leads limit fault current
during experimentation; they do not materially affect AT28C256 timing at these
rates.

## Safe automatic handoff

Entering PROGRAM mode:

1. Stop trace and make every Mega bus/control pin an input.
2. Pull 6502 `/RESET` low.
3. Pull W65C02S `BE` low.
4. Drive D7 `/OE` and D8 `/WE` high.
5. Drive A15 high; the existing inverter selects EEPROM `/CE`.
6. Drive the remaining address bus and begin EEPROM operations.

Returning to DEBUG mode:

1. Drive `/OE` and `/WE` high and release the address/data bus.
2. Return programmer controls to inputs; the D7 pull-down restores `/OE = LOW`.
3. Release `BE`.
4. Release `/RESET`, restarting the 6502 at the new reset vector.

The Mega asserts `/RESET` and `BE` open-drain style and releases them to their
external pull-ups. On a Mega reset, all shared pins revert to inputs.

## Build and upload

Install PlatformIO, then:

```bash
cd "6502/mega-programmer"
pio run
pio run --target upload
```

The serial monitor baud is 500000. Close PlatformIO's serial monitor before
using the host application because only one process can own the port.

## Install and launch the GUI

On macOS, install a current Tk runtime. Do not launch the GUI with
`/usr/bin/python3`; Apple's bundled Tk 8.5 can produce a completely blank
window in dark appearance.

```bash
brew install python@3.12 python-tk@3.12
cd "6502/mega-programmer"
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
python tools/eeprom_tool.py
```

Choose the Mega's serial port and click **Connect**. **Debug / Run** releases
the 6502. Debug trace is deliberately off by default; click **Start Bus Trace**
only when you want samples and **Stop Bus Trace** when finished. **EEPROM
Programmer** holds the CPU and gives the Mega the bus. Program, verify,
range-read and full-dump actions are available in that mode.

The trace path has hard resource limits:

- firmware output is capped at 200 lines per second;
- the GUI queue holds at most 512 events and drops excess trace samples;
- the text console retains at most 2,000 lines;
- the GUI processes a bounded event batch on each Tk update.

At a 1 MHz 6502 clock this is a representative live sample, not a lossless
cycle-by-cycle capture. Lossless 1 MHz tracing requires dedicated capture
hardware or on-device RAM buffering rather than a text GUI.

## CLI examples

```bash
python tools/eeprom_tool.py ports
python tools/eeprom_tool.py --port /dev/cu.usbmodemXXXX info
python tools/eeprom_tool.py --port /dev/cu.usbmodemXXXX mode program
python tools/eeprom_tool.py --port /dev/cu.usbmodemXXXX trace on
python tools/eeprom_tool.py --port /dev/cu.usbmodemXXXX trace off
python tools/eeprom_tool.py --port /dev/cu.usbmodemXXXX write rom.bin
python tools/eeprom_tool.py --port /dev/cu.usbmodemXXXX verify rom.bin
python tools/eeprom_tool.py --port /dev/cu.usbmodemXXXX dump backup.bin
python tools/eeprom_tool.py --port /dev/cu.usbmodemXXXX mode debug
```

For a ROM image that belongs at CPU `$8000`, use the default EEPROM offset
`0x0000`. A short image changes only its own bytes. To deliberately fill the
rest of the EEPROM:

```bash
python tools/eeprom_tool.py write rom.bin --fill 0xFF
```

See `PROTOCOL.md` for the serial framing details.
