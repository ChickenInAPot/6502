# Validation

Checked while preparing the repository in September 2026, on macOS.

| Check | Result |
| --- | --- |
| `pio run -d mega-programmer -e standalone_eeprom` | Passed |
| `pio run -d mega-programmer -e incircuit_ben_eater` | Passed |
| `pio run -d shift-register-programmer` | Passed |
| `make -C mega-programmer/rom` | Passed |
| ROM image size and reset vector | 32,768 bytes; reset vector `$8000` |
| Both Python host tools, `--help` | Passed |

The firmware builds used PlatformIO's Atmel AVR 5.0.0 platform and the Arduino Mega 2560 target. The ROM was assembled and linked with ca65/ld65. The Arduino framework emitted unused-parameter warnings in its own `new.cpp`; the builds completed successfully.

These checks did not upload firmware, write an EEPROM, verify wiring, exercise the GUI with a board, or measure transfer speed. The command-line help check establishes that the host tools load; it does not validate serial communication. Hardware behavior needs to be checked on the corresponding assembly.
