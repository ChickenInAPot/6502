# Standalone AT28C256 to Arduino Mega wiring

This wiring is only for testing and programming the EEPROM by itself. Do not
connect the 6502, VIA, RAM, address decoder, inverter, or oscillator yet.

View the 28-pin PDIP from above with its notch pointing upward.

| AT28C256 pin | Signal | Arduino Mega connection |
|---:|---|---:|
| 1 | A14 | D24 |
| 2 | A12 | D28 |
| 3 | A7 | D38 |
| 4 | A6 | D40 |
| 5 | A5 | D42 |
| 6 | A4 | D44 |
| 7 | A3 | D46 |
| 8 | A2 | D48 |
| 9 | A1 | D50 |
| 10 | A0 | D52 |
| 11 | I/O0 | D53 |
| 12 | I/O1 | D51 |
| 13 | I/O2 | D49 |
| 14 | GND | Mega GND |
| 15 | I/O3 | D47 |
| 16 | I/O4 | D45 |
| 17 | I/O5 | D43 |
| 18 | I/O6 | D41 |
| 19 | I/O7 | D39 |
| 20 | /CE | D6 and 10 kOhm pull-up to +5 V |
| 21 | A10 | D32 |
| 22 | /OE | D7 and 10 kOhm pull-up to +5 V |
| 23 | A11 | D30 |
| 24 | A9 | D34 |
| 25 | A8 | D36 |
| 26 | A13 | D26 |
| 27 | /WE | D8 and 10 kOhm pull-up to +5 V |
| 28 | VCC | Mega +5 V |

Address order summarized:

```text
EEPROM A14..A0
Mega   24,26,28,30,32,34,36,38,40,42,44,46,48,50,52
```

Data order summarized:

```text
EEPROM I/O7..I/O0
Mega   39,41,43,45,47,49,51,53
```

Add a 0.1 uF ceramic capacitor directly between EEPROM pins 28 and 14. All
three control pull-ups are required so `/CE`, `/OE`, and `/WE` default high
while the Mega resets. Mega D22, D4, D5, and D9 are unused in standalone mode.

Use the Mega's regulated +5 V and GND for the EEPROM. Never insert, remove, or
rewire the EEPROM while power is connected.
