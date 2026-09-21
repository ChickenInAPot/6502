#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Arduino Mega 2560 pin assignment
// ---------------------------------------------------------------------------
// Digital pins 22..29 are the complete PORTA register in bit order. Connecting
// EEPROM I/O0..I/O7 in that same order lets the firmware read or write all
// eight bits with one AVR instruction instead of eight digitalRead calls.
constexpr uint8_t DATA_FIRST_PIN = 22;
constexpr uint8_t DATA_LAST_PIN = 29;

// The Mega hardware SPI pins drive two cascaded 74HC595 address registers.
constexpr uint8_t SHIFT_DATA_PIN = 51;   // MOSI -> SER
constexpr uint8_t SHIFT_CLOCK_PIN = 52;  // SCK -> SRCLK
constexpr uint8_t SHIFT_LATCH_PIN = 53;  // SS -> RCLK

// AT28C256 active-low controls. Add a 10 kOhm pull-up from each signal to 5 V
// so the EEPROM cannot receive a stray write pulse while the Mega is resetting.
constexpr uint8_t EEPROM_WE_PIN = 30;
constexpr uint8_t EEPROM_OE_PIN = 31;
constexpr uint8_t EEPROM_CE_PIN = 32;

constexpr uint32_t SERIAL_BAUD = 250000;  // exact baud divisor at 16 MHz
constexpr uint32_t SHIFT_CLOCK_HZ = 8000000;
constexpr uint16_t EEPROM_SIZE = 32768U;
constexpr uint8_t EEPROM_PAGE_SIZE = 64;
constexpr uint32_t WRITE_TIMEOUT_US = 20000;
