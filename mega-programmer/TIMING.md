# AT28C256 timing and optimization notes

Source: the supplied Microchip AT28C256 data sheet, DS20006386B.

## Datasheet limits used by the firmware

| Requirement | Data sheet | Firmware behavior |
|---|---:|---|
| Read access | 150 ns maximum | 4 explicit NOPs (250 ns) after the last address-port update |
| Address hold after write | 50 ns minimum | loop/control overhead is several AVR cycles |
| Data setup before write | 50 ns minimum | port update plus explicit NOP before `/WE` low |
| `/WE` low pulse | 100 ns minimum | compiled low pulse is approximately 0.8 us at 16 MHz |
| `/WE` high between bytes | 50 ns minimum | address/data loop overhead greatly exceeds this |
| Time between page bytes | 150 us maximum | dirty offsets are compacted before loading; the timed loop contains no mask scan |
| Page size | 1-64 bytes | host frames are split on every 64-byte boundary |
| Standard write cycle | 10 ms maximum | D7/D6 polling, with a 20 ms fault timeout |
| AT28C256F write cycle | 3 ms maximum | same polling ends as soon as the chip is ready |

The page-write requirements are in Tables 6-3 and 6-4 and Figures 6-9,
6-15, and 6-17 of the supplied PDF.

## Optimization choices

1. **Parallel direct ports.** The original wiring spans several Mega ports, but
   every address and data operation is still compiled to masked AVR register
   access. No programming pulse uses Arduino `digitalWrite()`.
2. **Physical page writes.** Up to 64 dirty bytes share one internal EEPROM
   programming cycle. A naive fixed 10 ms byte writer takes at least 327.68
   seconds for 32 KiB; this design needs at most 512 page cycles.
3. **Sparse-page compaction.** Changed offsets are collected before `/WE`
   pulsing begins. Widely separated changes cannot consume the 150 us page-load
   window while the AVR scans unchanged offsets.
4. **Incremental comparison.** The Mega reads each incoming page and omits
   already-matching bytes. The data sheet confirms unspecified page bytes are
   not cycled.
5. **Ready polling.** D7 detects successful completion. D6 toggle polling also
   detects completion when software data protection rejected an ordinary
   write, allowing an immediate protected retry.
6. **Protected-write learning.** The firmware first preserves an unprotected
   chip's state. If verification shows SDP is active, it retries with
   `AA@5555`, `55@2AAA`, `A0@5555` and remembers that requirement.
7. **Bounded serial frames.** A 64-byte page plus CRC fits the Mega's limited
   SRAM and prevents a USB/UART stream from overrunning the 64-byte receive
   ring while the EEPROM is internally busy.
8. **500 kbaud.** This is an exact ATmega2560 baud divisor. Faster host code
   cannot reduce the EEPROM's internal write time; Python already supplies
   data much faster than the device can program it.

## Expected full-image time

- EEPROM internal maximum: `512 pages × 10 ms = 5.12 s`
- Raw 32 KiB payload at 500 kbaud, 8N1: about `0.66 s`
- Page headers, acknowledgements, comparison, polling and verification add
  overhead, so a fully different standard part should normally complete in
  several seconds rather than minutes.
- An AT28C256F or an image with few changed pages completes sooner.

Actual time depends on the specific EEPROM, USB serial bridge, wiring
capacitance, and number of changed pages. The final `@OK` response reports the
firmware-measured `elapsed_ms`.
