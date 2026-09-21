# Shift-register AT28C256 programmer

This PlatformIO project programs a 32 KiB AT28C256 using an Arduino Mega 2560
and two 74HC595 shift registers. Everything is 5 V, so the EEPROM data pins
connect directly to eight adjacent Mega pins—no level shifter and very little
extra wiring.

The firmware:

- drives the shift registers through 8 MHz hardware SPI;
- accesses I/O0-I/O7 as the Mega's complete 8-bit PORTA register;
- compares every requested byte with the actual EEPROM;
- writes only changed positions using 64-byte page operations;
- polls I/O7 for completion instead of delaying 10 ms after every byte;
- verifies every changed byte;
- remembers the last successful range and CRC-32 in the Mega's internal EEPROM;
- automatically handles an AT28C256 that already has software data protection.

There is no whole-chip erase. Matching bytes are never rewritten.

## Parts

- Arduino Mega 2560
- AT28C256, 28-pin PDIP
- 2 x 74HC595 shift registers
- 3 x 100 nF ceramic bypass capacitors
- 1 x 10 uF capacitor for the 5 V rail near the EEPROM
- 3 x 10 kOhm resistors
- 28-pin socket or ZIF socket, breadboard, and jumpers

All ICs run from the Mega's 5 V pin and share its GND. Place one 100 nF
capacitor directly across VCC and GND at each 74HC595 and at the AT28C256.

## Connections

```text
Mega 51 MOSI ----> LOW 74HC595 ----cascade----> HIGH 74HC595
                         | A0..A7                    | A8..A14
                         +--------> AT28C256 <-------+

Mega 22..29 <---------------------> AT28C256 I/O0..I/O7
Mega 30,31,32 ---------------------> AT28C256 /WE,/OE,/CE
```

`LOW` means the shift register nearest the Mega. `HIGH` means the second one.

## Wire the two 74HC595s

| 74HC595 pin | Connection |
|---|---|
| 16 VCC (both) | Mega 5 V |
| 8 GND (both) | Mega GND |
| 10 /SRCLR (both) | Mega 5 V |
| 13 /OE (both) | Mega GND |
| 11 SRCLK (both) | Mega pin 52 (SCK) |
| 12 RCLK (both) | Mega pin 53 |
| 14 SER (LOW) | Mega pin 51 (MOSI) |
| 9 QH' (LOW) | pin 14 SER of HIGH register |
| QA..QH (LOW) | EEPROM A0..A7 |
| QA..QG (HIGH) | EEPROM A8..A14 |
| QH (HIGH) | not connected |

On a 74HC595, QA is pin 15, QB through QH are pins 1 through 7, and QH' is pin
9. Keep the three SPI wires short on a breadboard.

## Wire the AT28C256

This table is for the 28-pin PDIP viewed from above with the notch at the top.

| EEPROM pin | Signal | Connection |
|---:|---|---|
| 1 | A14 | HIGH 74HC595 QG |
| 2 | A12 | HIGH 74HC595 QE |
| 3 | A7 | LOW 74HC595 QH |
| 4 | A6 | LOW 74HC595 QG |
| 5 | A5 | LOW 74HC595 QF |
| 6 | A4 | LOW 74HC595 QE |
| 7 | A3 | LOW 74HC595 QD |
| 8 | A2 | LOW 74HC595 QC |
| 9 | A1 | LOW 74HC595 QB |
| 10 | A0 | LOW 74HC595 QA |
| 11 | I/O0 | Mega pin 22 |
| 12 | I/O1 | Mega pin 23 |
| 13 | I/O2 | Mega pin 24 |
| 14 | GND | Mega GND |
| 15 | I/O3 | Mega pin 25 |
| 16 | I/O4 | Mega pin 26 |
| 17 | I/O5 | Mega pin 27 |
| 18 | I/O6 | Mega pin 28 |
| 19 | I/O7 | Mega pin 29 |
| 20 | /CE | Mega pin 32 plus 10 kOhm to 5 V |
| 21 | A10 | HIGH 74HC595 QC |
| 22 | /OE | Mega pin 31 plus 10 kOhm to 5 V |
| 23 | A11 | HIGH 74HC595 QD |
| 24 | A9 | HIGH 74HC595 QB |
| 25 | A8 | HIGH 74HC595 QA |
| 26 | A13 | HIGH 74HC595 QF |
| 27 | /WE | Mega pin 30 plus 10 kOhm to 5 V |
| 28 | VCC | Mega 5 V |

The three 10 kOhm pull-ups keep /CE, /OE, and /WE inactive while the Mega is
resetting. Never insert or remove the EEPROM while power is connected.

## Build and upload

Open `6502/shift-register-programmer` in PlatformIO and select
**Upload**, or run:

```bash
cd 6502/shift-register-programmer
pio run
pio run --target upload
```

## Install the computer-side tool

```bash
cd 6502/shift-register-programmer
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
```

Write a ROM image starting at address zero:

```bash
python tools/eeprom.py write rom.bin
```

Bytes after a short file remain unchanged. Use `--fill 0xFF` only when you
actually want the rest of the chip changed to `0xFF`:

```bash
python tools/eeprom.py write rom.bin --fill 0xFF
```

Read, verify, and inspect remembered metadata:

```bash
python tools/eeprom.py verify rom.bin
python tools/eeprom.py dump backup.bin
python tools/eeprom.py read region.bin --offset 0x6000 --length 0x2000
python tools/eeprom.py info
```

If several serial devices are attached, put the port before the command:

```bash
python tools/eeprom.py --port /dev/cu.usbmodemXXXX write rom.bin
```

## Remembered upload

Normal writes always compare with the physical EEPROM, which remains correct if
you swap chips. The Mega additionally stores the last successful start address,
length, and CRC-32 in its own internal EEPROM.

`--trust-cache` makes an identical request return immediately. Use it only if
the same AT28C256 has remained installed and nothing else has modified it. The
cache record is rewritten only when its values change, reducing wear on the
Mega's internal EEPROM.

## Estimated write time

The old reference firmware slept for 10 ms after every byte, making a full
32 KiB upload take more than five minutes. This firmware groups up to 64 changed
bytes into each internal write cycle. A completely different image needs at
most 512 page cycles—about 5.1 seconds of worst-case EEPROM programming time,
plus comparison, serial transfer, and verification. Small code changes usually
touch only a few pages.

## Troubleshooting

- **Every byte reads `0xFF`:** check /CE, /OE, GND, and the data wires on Mega
  pins 22 through 29.
- **Addresses repeat every 256 bytes:** check the HIGH shift register cascade
  and EEPROM A8 through A14.
- **Intermittent verification errors:** add the three bypass capacitors, shorten
  the SPI wires, and confirm all ICs use the same 5 V and GND rails.
- **Upload will not start:** close PlatformIO Serial Monitor before running the
  Python tool; only one program can own the Mega's serial port.
- **Result contains `sdp=1`:** the AT28C256 already had software data protection;
  the firmware detected it and used the protected-program sequence.
