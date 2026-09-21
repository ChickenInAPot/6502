# Serial protocol

The host and Arduino Mega communicate at 250000 baud. Commands and status
messages are ASCII lines terminated by `\n`. Integers are decimal except CRC-32
values, which are hexadecimal. CRC-32 uses the standard reflected polynomial
implemented by Python's `zlib.crc32`.

## Commands

- `PING` -> `OK firmware=2.0 protocol=2`
- `INFO` -> one `OK ...` metadata line
- `READ start length` -> `DATA length crc32`, exactly `length` raw bytes, then
  `\nOK\n`
- `WRITE start length crc32 [TRUST]` begins a page-framed upload.

## Page-framed write

The Mega has 8 KiB of SRAM, so the host never sends an unbounded binary stream.
After `WRITE`, the Mega normally returns `READY`. The host then repeats:

1. Send `PAGE address length crc32`. Length is 1 to 64 and the range must not
   cross an AT28C256 page boundary.
2. Wait for `SEND`.
3. Send exactly `length` raw bytes.
4. Wait for `ACK received=... changed=... pages=...`.

After the declared image length has been acknowledged, the Mega returns one
final `OK ...` or `ERR ...` line. Per-page CRC protects every binary block, and
the original `WRITE` CRC protects the complete image.

The optional `TRUST` flag may return `OK ... cache_hit=1` instead of `READY` if
the requested range and image CRC match the record stored in the Mega's internal
EEPROM. The host must not send page frames after a cache hit.
