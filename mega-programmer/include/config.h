#pragma once

#include <Arduino.h>

#ifndef STANDALONE_EEPROM
#define STANDALONE_EEPROM 1
#endif

// Host link. 500000 baud is exact on a 16 MHz ATmega2560 and is conservative
// enough for the Mega's USB serial bridge.
constexpr uint32_t SERIAL_BAUD = 500000UL;

// AT28C256 geometry.
constexpr uint16_t EEPROM_SIZE = 32768U;
constexpr uint8_t EEPROM_PAGE_SIZE = 64U;
constexpr uint32_t WRITE_TIMEOUT_US = 20000UL;

// The EEPROM occupies $8000-$FFFF in the 6502 address space. The programmer's
// host protocol uses logical offsets $0000-$7FFF and the firmware forces A15
// high while accessing the chip.
constexpr uint16_t EEPROM_SYSTEM_BASE = 0x8000U;

// Existing debug inputs.
constexpr uint8_t CLOCK_PIN = 2;       // PE4 / INT4
constexpr uint8_t READ_WRITE_PIN = 3;  // PE5

// Open-drain ownership controls. Each requires an external 10 kOhm pull-up to
// +5 V. The Mega asserts the signal by driving LOW and releases it by becoming
// an INPUT with its internal pull-up disabled.
constexpr uint8_t CPU_RESET_PIN = 4;  // PG5
constexpr uint8_t CPU_BE_PIN = 5;     // PE3

// EEPROM controls.
//
// Standalone build:
//   D6 -> /CE, D7 -> /OE, D8 -> /WE.
//
// In-circuit Ben Eater build:
//   /CE remains driven by the A15 inverter; D6 is disconnected.
//   D7 connects directly to /OE, with a 10 kOhm pull-down for RUN mode.
//   D8 connects directly to /WE, with a 10 kOhm pull-up.
constexpr uint8_t EEPROM_CE_PIN = 6;          // PH3, standalone only
constexpr uint8_t EEPROM_OE_PIN = 7;          // PH4
constexpr uint8_t EEPROM_WE_PIN = 8;          // PH5

// These are the bus connections already used by the original debugger:
//
//   A15..A0: 22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52
//   D7..D0:  39,41,43,45,47,49,51,53
//
// main.cpp manipulates their AVR ports directly, so changing that wiring also
// requires changing the masks and bit mapping there.
