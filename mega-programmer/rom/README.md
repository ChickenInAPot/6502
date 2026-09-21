# Counting ROM

`counting.s` is a W65C02 test program for the standard Ben Eater memory map.
It starts at `$8000`, configures all eight W65C22 Port B pins as outputs, and
increments a binary count about ten times per second at a 1 MHz CPU clock.

The complete 32 KiB image is `build/counting.bin`. Unused ROM locations contain
the 6502 NOP opcode `$EA`. The vector bytes are:

```text
$FFFA-$FFFB: NMI   -> handler containing RTI
$FFFC-$FFFD: RESET -> $8000
$FFFE-$FFFF: IRQ   -> handler containing RTI
```

Build:

```bash
cd rom
make
```

Program the standalone EEPROM at offset `0x0000`:

```bash
../.venv/bin/python ../tools/eeprom_tool.py \
  --port /dev/cu.usbmodemXXXX \
  write build/counting.bin
```

Verify it:

```bash
../.venv/bin/python ../tools/eeprom_tool.py \
  --port /dev/cu.usbmodemXXXX \
  verify build/counting.bin
```
