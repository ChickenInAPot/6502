# Mega serial protocol

The firmware and host communicate at 500000 baud. Commands and responses are
ASCII lines terminated by LF. Firmware responses begin with `@`, so debugger
bus samples cannot be mistaken for protocol traffic.

## General commands

- `PING` -> `@OK firmware=3.0 protocol=3 mode=...`
- `INFO` -> `@OK board=mega2560 model=AT28C256 ...`
- `MODE PROGRAM` -> assert 6502 `/RESET` and `BE`, then take the bus
- `MODE DEBUG` -> release the bus and restart the 6502
- `TRACE ON` -> start a bounded, maximum 200-line/second live sample
- `TRACE OFF` -> stop live samples; this is the power-on default
- `RESET` -> pulse the 6502 reset line while in debug mode

## Read

Send `READ start length`. The firmware replies:

```text
@DATA length
<exactly length raw bytes>
@OK crc32=...
```

The host validates the CRC-32 after receiving the raw bytes.

## Write

Send `WRITE start length crc32`. The firmware responds `@READY`. Split the
image at physical 64-byte EEPROM page boundaries and repeat:

1. Send `PAGE address length crc32`.
2. Wait for `@SEND`.
3. Send exactly `length` raw bytes.
4. Wait for `@ACK received=... changed=... pages=...`.

The final response is `@OK ...` or `@ERR ...`. A page CRC protects each serial
block, an image CRC protects the complete transfer, and changed bytes are read
back from the EEPROM after each physical page write.
