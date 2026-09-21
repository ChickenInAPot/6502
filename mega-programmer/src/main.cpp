#include <Arduino.h>
#include <avr/interrupt.h>
#include <util/delay_basic.h>

#include "config.h"

namespace {

enum class OperatingMode : uint8_t { DEBUG, PROGRAM };

constexpr uint8_t ADDR_MASK_A = 0x55;  // PA0,2,4,6 = A15..A12
constexpr uint8_t ADDR_MASK_C = 0xAA;  // PC7,5,3,1 = A11..A8
constexpr uint8_t ADDR_MASK_D = _BV(PD7);  // PD7 = A7
constexpr uint8_t ADDR_MASK_G = _BV(PG1);  // PG1 = A6
constexpr uint8_t ADDR_MASK_L = 0xAA;      // PL7,5,3,1 = A5..A2
constexpr uint8_t ADDR_MASK_B = _BV(PB3) | _BV(PB1);  // PB3,1 = A1,A0

constexpr uint8_t DATA_MASK_G = _BV(PG2) | _BV(PG0);  // D7,D6
constexpr uint8_t DATA_MASK_L = 0x55;                  // D5..D2
constexpr uint8_t DATA_MASK_B = _BV(PB2) | _BV(PB0);  // D1,D0

#if STANDALONE_EEPROM
constexpr uint8_t CONTROL_MASK =
    _BV(PH3) | _BV(PH4) | _BV(PH5);  // /CE, /OE, /WE
constexpr uint8_t CE_BIT = _BV(PH3);
#else
constexpr uint8_t CONTROL_MASK = _BV(PH4) | _BV(PH5);  // /OE, /WE
#endif
constexpr uint8_t OE_BIT = _BV(PH4);
constexpr uint8_t WE_BIT = _BV(PH5);

constexpr uint32_t DEBUG_OUTPUT_INTERVAL_US = 5000UL;  // Hard limit: 200 lines/s.

struct BusSample {
  uint16_t address;
  uint8_t data;
  uint8_t read;
};

volatile OperatingMode operatingMode = OperatingMode::DEBUG;
bool traceEnabled = false;
bool previousClockHigh = false;
bool debugSamplePending = false;
BusSample latestDebugSample{};

uint8_t pageBuffer[EEPROM_PAGE_SIZE];
String commandBuffer;
bool softwareProtectionRequired = false;

inline void writeMasked(volatile uint8_t &port, uint8_t mask, uint8_t value) {
  port = static_cast<uint8_t>((port & ~mask) | (value & mask));
}

inline uint8_t addressPortA(uint16_t address) {
  return ((address & 0x8000U) ? _BV(PA0) : 0) |
         ((address & 0x4000U) ? _BV(PA2) : 0) |
         ((address & 0x2000U) ? _BV(PA4) : 0) |
         ((address & 0x1000U) ? _BV(PA6) : 0);
}

inline uint8_t addressPortC(uint16_t address) {
  return ((address & 0x0800U) ? _BV(PC7) : 0) |
         ((address & 0x0400U) ? _BV(PC5) : 0) |
         ((address & 0x0200U) ? _BV(PC3) : 0) |
         ((address & 0x0100U) ? _BV(PC1) : 0);
}

inline uint8_t addressPortL(uint16_t address) {
  return ((address & 0x0020U) ? _BV(PL7) : 0) |
         ((address & 0x0010U) ? _BV(PL5) : 0) |
         ((address & 0x0008U) ? _BV(PL3) : 0) |
         ((address & 0x0004U) ? _BV(PL1) : 0);
}

inline void setSystemAddress(uint16_t address) {
  writeMasked(PORTA, ADDR_MASK_A, addressPortA(address));
  writeMasked(PORTC, ADDR_MASK_C, addressPortC(address));
  writeMasked(PORTD, ADDR_MASK_D, (address & 0x0080U) ? ADDR_MASK_D : 0);
  writeMasked(PORTG, ADDR_MASK_G, (address & 0x0040U) ? ADDR_MASK_G : 0);
  writeMasked(PORTL, ADDR_MASK_L, addressPortL(address));
  writeMasked(PORTB, ADDR_MASK_B,
              ((address & 0x0002U) ? _BV(PB3) : 0) |
                  ((address & 0x0001U) ? _BV(PB1) : 0));
}

inline void setEepromAddress(uint16_t logicalAddress) {
  setSystemAddress(static_cast<uint16_t>(EEPROM_SYSTEM_BASE | logicalAddress));
}

inline uint16_t sampleSystemAddress() {
  uint16_t address = 0;
  address |= (PINA & _BV(PA0)) ? 0x8000U : 0;
  address |= (PINA & _BV(PA2)) ? 0x4000U : 0;
  address |= (PINA & _BV(PA4)) ? 0x2000U : 0;
  address |= (PINA & _BV(PA6)) ? 0x1000U : 0;
  address |= (PINC & _BV(PC7)) ? 0x0800U : 0;
  address |= (PINC & _BV(PC5)) ? 0x0400U : 0;
  address |= (PINC & _BV(PC3)) ? 0x0200U : 0;
  address |= (PINC & _BV(PC1)) ? 0x0100U : 0;
  address |= (PIND & _BV(PD7)) ? 0x0080U : 0;
  address |= (PING & _BV(PG1)) ? 0x0040U : 0;
  address |= (PINL & _BV(PL7)) ? 0x0020U : 0;
  address |= (PINL & _BV(PL5)) ? 0x0010U : 0;
  address |= (PINL & _BV(PL3)) ? 0x0008U : 0;
  address |= (PINL & _BV(PL1)) ? 0x0004U : 0;
  address |= (PINB & _BV(PB3)) ? 0x0002U : 0;
  address |= (PINB & _BV(PB1)) ? 0x0001U : 0;
  return address;
}

inline uint8_t dataPortG(uint8_t data) {
  return ((data & 0x80U) ? _BV(PG2) : 0) |
         ((data & 0x40U) ? _BV(PG0) : 0);
}

inline uint8_t dataPortL(uint8_t data) {
  return ((data & 0x20U) ? _BV(PL6) : 0) |
         ((data & 0x10U) ? _BV(PL4) : 0) |
         ((data & 0x08U) ? _BV(PL2) : 0) |
         ((data & 0x04U) ? _BV(PL0) : 0);
}

inline void setData(uint8_t data) {
  writeMasked(PORTG, DATA_MASK_G, dataPortG(data));
  writeMasked(PORTL, DATA_MASK_L, dataPortL(data));
  writeMasked(PORTB, DATA_MASK_B,
              ((data & 0x02U) ? _BV(PB2) : 0) |
                  ((data & 0x01U) ? _BV(PB0) : 0));
}

inline uint8_t sampleData() {
  uint8_t data = 0;
  data |= (PING & _BV(PG2)) ? 0x80U : 0;
  data |= (PING & _BV(PG0)) ? 0x40U : 0;
  data |= (PINL & _BV(PL6)) ? 0x20U : 0;
  data |= (PINL & _BV(PL4)) ? 0x10U : 0;
  data |= (PINL & _BV(PL2)) ? 0x08U : 0;
  data |= (PINL & _BV(PL0)) ? 0x04U : 0;
  data |= (PINB & _BV(PB2)) ? 0x02U : 0;
  data |= (PINB & _BV(PB0)) ? 0x01U : 0;
  return data;
}

void addressBusInput() {
  DDRA &= ~ADDR_MASK_A;
  DDRC &= ~ADDR_MASK_C;
  DDRD &= ~ADDR_MASK_D;
  DDRG &= ~ADDR_MASK_G;
  DDRL &= ~ADDR_MASK_L;
  DDRB &= ~ADDR_MASK_B;
  PORTA &= ~ADDR_MASK_A;
  PORTC &= ~ADDR_MASK_C;
  PORTD &= ~ADDR_MASK_D;
  PORTG &= ~ADDR_MASK_G;
  PORTL &= ~ADDR_MASK_L;
  PORTB &= ~ADDR_MASK_B;
}

void addressBusOutput() {
  DDRA |= ADDR_MASK_A;
  DDRC |= ADDR_MASK_C;
  DDRD |= ADDR_MASK_D;
  DDRG |= ADDR_MASK_G;
  DDRL |= ADDR_MASK_L;
  DDRB |= ADDR_MASK_B;
}

void dataBusInput() {
  DDRG &= ~DATA_MASK_G;
  DDRL &= ~DATA_MASK_L;
  DDRB &= ~DATA_MASK_B;
  PORTG &= ~DATA_MASK_G;
  PORTL &= ~DATA_MASK_L;
  PORTB &= ~DATA_MASK_B;
}

void dataBusOutput() {
  DDRG |= DATA_MASK_G;
  DDRL |= DATA_MASK_L;
  DDRB |= DATA_MASK_B;
}

inline void controlsHigh(uint8_t bits) { PORTH |= bits; }
inline void controlsLow(uint8_t bits) { PORTH &= ~bits; }

void controlsInput() {
  // In the direct-/OE in-circuit build, an external 10 kOhm pull-down enables
  // EEPROM reads as soon as the Mega releases D7. /WE has a pull-up.
  DDRH &= ~CONTROL_MASK;
  PORTH &= ~CONTROL_MASK;
}

void controlsOutputInactive() {
#if STANDALONE_EEPROM
  PORTH |= CONTROL_MASK;
  DDRH |= CONTROL_MASK;
#else
  // Preload /OE and /WE high before making either pin an output. This disables
  // EEPROM data output and write pulses before the Mega drives the buses.
  PORTH |= OE_BIT | WE_BIT;
  DDRH |= CONTROL_MASK;
#endif
}

void assertCpuReset() {
  PORTG &= ~_BV(PG5);
  DDRG |= _BV(PG5);
}

void releaseCpuReset() {
  DDRG &= ~_BV(PG5);
  PORTG &= ~_BV(PG5);
}

void assertCpuBusEnable() {
  PORTE &= ~_BV(PE3);
  DDRE |= _BV(PE3);
}

void releaseCpuBusEnable() {
  DDRE &= ~_BV(PE3);
  PORTE &= ~_BV(PE3);
}

void clearDebugQueue() {
  debugSamplePending = false;
  previousClockHigh = (PINE & _BV(PE4)) != 0;
}

void enterProgramMode() {
  if (operatingMode == OperatingMode::PROGRAM) {
    return;
  }

  operatingMode = OperatingMode::PROGRAM;
  traceEnabled = false;
  clearDebugQueue();

  // Release everything before taking ownership. /RESET stops execution and BE
  // then makes the W65C02S address, data and RWB outputs high impedance.
  addressBusInput();
  dataBusInput();
  controlsInput();
  assertCpuReset();
  delayMicroseconds(10);
  assertCpuBusEnable();
  delayMicroseconds(10);

  controlsOutputInactive();
  setEepromAddress(0);
  addressBusOutput();
  dataBusInput();
}

void enterDebugMode() {
  if (operatingMode == OperatingMode::DEBUG) {
    traceEnabled = false;
    clearDebugQueue();
    return;
  }

  // Inhibit the EEPROM before releasing every Mega bus connection.
  controlsHigh(CONTROL_MASK);
  dataBusInput();
  addressBusInput();
  controlsInput();

  releaseCpuBusEnable();
  delayMicroseconds(10);
  releaseCpuReset();

  clearDebugQueue();
  operatingMode = OperatingMode::DEBUG;
}

void pulseCpuReset() {
  if (operatingMode != OperatingMode::DEBUG) {
    return;
  }
  assertCpuReset();
  delay(2);
  releaseCpuReset();
}

inline void readAccessDelay() {
  // The AT28C256 read access time is 150 ns maximum. Four NOPs are 250 ns at
  // 16 MHz, in addition to the time consumed by the final port write.
  __asm__ __volatile__("nop\n\tnop\n\tnop\n\tnop\n\t");
}

void beginRead() {
#if STANDALONE_EEPROM
  controlsHigh(CE_BIT | OE_BIT | WE_BIT);
#else
  controlsHigh(OE_BIT | WE_BIT);
#endif
  dataBusInput();
#if STANDALONE_EEPROM
  controlsLow(CE_BIT);
#endif
  controlsLow(OE_BIT);
}

uint8_t readByteInReadMode(uint16_t address) {
  setEepromAddress(address);
  readAccessDelay();
  return sampleData();
}

void endRead() {
#if STANDALONE_EEPROM
  controlsHigh(OE_BIT | CE_BIT);
#else
  controlsHigh(OE_BIT);
#endif
}

void readRange(uint16_t start, uint8_t *destination, uint8_t length) {
  beginRead();
  for (uint8_t index = 0; index < length; ++index) {
    destination[index] =
        readByteInReadMode(static_cast<uint16_t>(start + index));
  }
  endRead();
}

void beginWrite() {
#if STANDALONE_EEPROM
  controlsHigh(CE_BIT | OE_BIT | WE_BIT);
#else
  controlsHigh(OE_BIT | WE_BIT);
#endif
  dataBusOutput();
#if STANDALONE_EEPROM
  controlsLow(CE_BIT);
#endif
}

inline void loadWriteByte(uint16_t address, uint8_t data) {
  setEepromAddress(address);
  setData(data);

  // Port updates above already exceed the 50 ns address/data setup time.
  __asm__ __volatile__("nop\n\t");
  controlsLow(WE_BIT);
  delayMicroseconds(1);
  controlsHigh(WE_BIT);
}

bool waitForWrite(uint16_t lastAddress, uint8_t lastData) {
#if STANDALONE_EEPROM
  // Use the AT28C256's 10 ms maximum page-write time plus margin. This
  // deliberately favors reliability while diagnosing a direct breadboard
  // connection; unchanged pages are still skipped by programChunk().
  delay(12);
  dataBusInput();
  setEepromAddress(lastAddress);
  controlsLow(CE_BIT);
  controlsLow(OE_BIT);
  readAccessDelay();
  const bool complete = sampleData() == lastData;
  controlsHigh(OE_BIT | CE_BIT);
  return complete;
#else
  // Let the 150 us maximum byte-load window expire. This costs only 82 ms over
  // an entire 512-page chip and avoids relying on a read to close the window.
  delayMicroseconds(160);
  dataBusInput();
  setEepromAddress(lastAddress);
  controlsLow(OE_BIT);

  const uint32_t started = micros();
  readAccessDelay();
  uint8_t previous = sampleData();
  while (static_cast<uint32_t>(micros() - started) < WRITE_TIMEOUT_US) {
    if ((previous & 0x80U) == (lastData & 0x80U)) {
      controlsHigh(OE_BIT);
      return true;
    }

    // A fresh read advances the D6 toggle bit. /OE high must last at least
    // 150 ns for toggle polling; the call plus three NOPs exceeds that.
    controlsHigh(OE_BIT);
    __asm__ __volatile__("nop\n\tnop\n\tnop\n\t");
    controlsLow(OE_BIT);
    readAccessDelay();
    const uint8_t current = sampleData();
    if (((current ^ previous) & 0x40U) == 0) {
      // The write cycle ended. Verification decides whether the data changed;
      // this path detects an SDP-rejected ordinary write without a timeout.
      controlsHigh(OE_BIT);
      return true;
    }
    previous = current;
  }
  controlsHigh(OE_BIT);
  return false;
#endif
}

bool verifyDirtyBytes(uint16_t pageStart, uint16_t sourceStart,
                      const uint8_t *source, uint64_t dirtyMask,
                      uint16_t &failedAddress) {
  beginRead();
  uint64_t bit = 1;
  for (uint8_t offset = 0; offset < EEPROM_PAGE_SIZE; ++offset, bit <<= 1) {
    if ((dirtyMask & bit) == 0) {
      continue;
    }
    const uint16_t address = static_cast<uint16_t>(pageStart + offset);
    const uint8_t expected = source[address - sourceStart];
    if (readByteInReadMode(address) != expected) {
      failedAddress = address;
      endRead();
      return false;
    }
  }
  endRead();
  return true;
}

bool programDirtyBytes(uint16_t pageStart, uint16_t sourceStart,
                       const uint8_t *source, uint64_t dirtyMask,
                       bool protectedWrite, uint16_t &failedAddress) {
  // Compact sparse dirty positions before the first /WE pulse. Scanning a
  // 64-bit mask between two page-load pulses is needlessly expensive on an
  // 8-bit AVR and could approach tBLC for widely separated changes.
  uint8_t changedOffsets[EEPROM_PAGE_SIZE];
  uint8_t changedCount = 0;
  uint64_t bit = 1;
  for (uint8_t offset = 0; offset < EEPROM_PAGE_SIZE; ++offset, bit <<= 1) {
    if (dirtyMask & bit) {
      changedOffsets[changedCount++] = offset;
    }
  }

  beginWrite();

  if (protectedWrite) {
    // Microchip's software-protected program prefix. These command addresses
    // are logical EEPROM addresses; setEepromAddress adds system A15.
    loadWriteByte(0x5555U, 0xAAU);
    loadWriteByte(0x2AAAU, 0x55U);
    loadWriteByte(0x5555U, 0xA0U);
  }

  uint16_t lastAddress = pageStart;
  uint8_t lastData = 0;
  for (uint8_t index = 0; index < changedCount; ++index) {
    const uint8_t offset = changedOffsets[index];
    lastAddress = static_cast<uint16_t>(pageStart + offset);
    lastData = source[lastAddress - sourceStart];
    loadWriteByte(lastAddress, lastData);
  }

  if (!waitForWrite(lastAddress, lastData)) {
    failedAddress = lastAddress;
    return false;
  }
  return verifyDirtyBytes(pageStart, sourceStart, source, dirtyMask,
                          failedAddress);
}

struct ProgramResult {
  bool ok;
  uint8_t changed;
  bool pageWritten;
  bool protectionDetected;
  uint16_t failedAddress;
};

ProgramResult programChunk(uint16_t start, const uint8_t *source,
                           uint8_t length) {
  ProgramResult result{true, 0, false, false, 0};
  const uint16_t pageStart =
      static_cast<uint16_t>(start & ~(EEPROM_PAGE_SIZE - 1U));
  uint64_t dirtyMask = 0;

  beginRead();
  for (uint8_t index = 0; index < length; ++index) {
    const uint16_t address = static_cast<uint16_t>(start + index);
    if (readByteInReadMode(address) != source[index]) {
      dirtyMask |= (uint64_t{1} << (address - pageStart));
      ++result.changed;
    }
  }
  endRead();

  if (dirtyMask == 0) {
    return result;
  }

  bool written =
      programDirtyBytes(pageStart, start, source, dirtyMask,
                        softwareProtectionRequired, result.failedAddress);
  if (!written && !softwareProtectionRequired) {
    written = programDirtyBytes(pageStart, start, source, dirtyMask, true,
                                result.failedAddress);
    if (written) {
      softwareProtectionRequired = true;
      result.protectionDetected = true;
    }
  }

  result.ok = written;
  result.pageWritten = written;
  return result;
}

uint32_t crc32Update(uint32_t state, const uint8_t *data, size_t length) {
  for (size_t index = 0; index < length; ++index) {
    state ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      state = (state >> 1) ^ (0xEDB88320UL & (0UL - (state & 1UL)));
    }
  }
  return state;
}

uint32_t crc32(const uint8_t *data, size_t length) {
  return ~crc32Update(0xFFFFFFFFUL, data, length);
}

bool receiveExact(uint8_t *destination, size_t length, uint32_t timeoutMs) {
  const uint32_t started = millis();
  size_t received = 0;
  while (received < length) {
    const int available = Serial.available();
    if (available > 0) {
      const size_t remaining = length - received;
      const size_t count =
          static_cast<size_t>(available) < remaining
              ? static_cast<size_t>(available)
              : remaining;
      received += Serial.readBytes(destination + received, count);
    } else if (static_cast<uint32_t>(millis() - started) >= timeoutMs) {
      return false;
    }
  }
  return true;
}

bool readCommandLine(String &line, uint32_t timeoutMs) {
  line = "";
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
    while (Serial.available()) {
      const char value = static_cast<char>(Serial.read());
      if (value == '\n') {
        line.trim();
        return true;
      }
      if (value != '\r' && line.length() < 100) {
        line += value;
      }
    }
  }
  return false;
}

void printOk(const __FlashStringHelper *message) {
  Serial.print(F("@OK "));
  Serial.println(message);
}

void printError(const __FlashStringHelper *message) {
  Serial.print(F("@ERR "));
  Serial.println(message);
}

void handleReadCommand(const char *line) {
  if (operatingMode != OperatingMode::PROGRAM) {
    printError(F("requires_program_mode"));
    return;
  }

  unsigned long start = 0;
  unsigned long length = 0;
  if (sscanf(line, "READ %lu %lu", &start, &length) != 2 || length == 0 ||
      start >= EEPROM_SIZE || start + length > EEPROM_SIZE) {
    printError(F("invalid_read_range"));
    return;
  }

  Serial.print(F("@DATA "));
  Serial.println(length);
  uint32_t state = 0xFFFFFFFFUL;
  uint32_t sent = 0;
  while (sent < length) {
    const uint8_t count =
        (length - sent) > EEPROM_PAGE_SIZE
            ? EEPROM_PAGE_SIZE
            : static_cast<uint8_t>(length - sent);
    readRange(static_cast<uint16_t>(start + sent), pageBuffer, count);
    state = crc32Update(state, pageBuffer, count);
    Serial.write(pageBuffer, count);
    sent += count;
  }
  Serial.print(F("\n@OK crc32="));
  Serial.println(~state, HEX);
}

void handleWriteCommand(const char *line) {
  if (operatingMode != OperatingMode::PROGRAM) {
    printError(F("requires_program_mode"));
    return;
  }

  unsigned long start = 0;
  unsigned long length = 0;
  unsigned long expectedCrc = 0;
  if (sscanf(line, "WRITE %lu %lu %lx", &start, &length, &expectedCrc) != 3 ||
      length == 0 || start >= EEPROM_SIZE ||
      start + length > EEPROM_SIZE) {
    printError(F("invalid_write_range"));
    return;
  }

  Serial.println(F("@READY"));
  const uint32_t started = millis();
  uint32_t rollingCrc = 0xFFFFFFFFUL;
  uint32_t received = 0;
  uint32_t changed = 0;
  uint16_t pagesWritten = 0;
  bool protectionDetected = false;

  while (received < length) {
    String pageLine;
    if (!readCommandLine(pageLine, 5000)) {
      printError(F("page_header_timeout"));
      return;
    }

    unsigned long pageAddress = 0;
    unsigned int pageLength = 0;
    unsigned long expectedPageCrc = 0;
    if (sscanf(pageLine.c_str(), "PAGE %lu %u %lx", &pageAddress, &pageLength,
               &expectedPageCrc) != 3 ||
        pageAddress != start + received || pageLength == 0 ||
        pageLength > EEPROM_PAGE_SIZE || pageLength > length - received ||
        (pageAddress / EEPROM_PAGE_SIZE) !=
            ((pageAddress + pageLength - 1U) / EEPROM_PAGE_SIZE)) {
      printError(F("invalid_page_frame"));
      return;
    }

    Serial.println(F("@SEND"));
    if (!receiveExact(pageBuffer, pageLength, 3000)) {
      printError(F("page_data_timeout"));
      return;
    }
    const uint32_t actualPageCrc = crc32(pageBuffer, pageLength);
    if (actualPageCrc != expectedPageCrc) {
      Serial.print(F("@ERR page_crc expected="));
      Serial.print(expectedPageCrc, HEX);
      Serial.print(F(" actual="));
      Serial.println(actualPageCrc, HEX);
      return;
    }

    rollingCrc = crc32Update(rollingCrc, pageBuffer, pageLength);
    const ProgramResult result =
        programChunk(static_cast<uint16_t>(pageAddress), pageBuffer,
                     static_cast<uint8_t>(pageLength));
    if (!result.ok) {
      Serial.print(F("@ERR verify_failed address=0x"));
      Serial.println(result.failedAddress, HEX);
      return;
    }

    changed += result.changed;
    pagesWritten += result.pageWritten ? 1U : 0U;
    protectionDetected |= result.protectionDetected;
    received += pageLength;
    Serial.print(F("@ACK received="));
    Serial.print(received);
    Serial.print(F(" changed="));
    Serial.print(changed);
    Serial.print(F(" pages="));
    Serial.println(pagesWritten);
  }

  const uint32_t actualCrc = ~rollingCrc;
  if (actualCrc != expectedCrc) {
    Serial.print(F("@ERR image_crc expected="));
    Serial.print(expectedCrc, HEX);
    Serial.print(F(" actual="));
    Serial.println(actualCrc, HEX);
    return;
  }

  Serial.print(F("@OK received="));
  Serial.print(received);
  Serial.print(F(" changed="));
  Serial.print(changed);
  Serial.print(F(" pages="));
  Serial.print(pagesWritten);
  Serial.print(F(" sdp="));
  Serial.print(protectionDetected ? 1 : 0);
  Serial.print(F(" crc32="));
  Serial.print(actualCrc, HEX);
  Serial.print(F(" elapsed_ms="));
  Serial.println(millis() - started);
}

void handleCommand(String command) {
  command.trim();
  if (command.length() == 0) {
    return;
  }

  if (command == F("PING")) {
    Serial.print(F("@OK firmware=3.0 protocol=3 mode="));
    Serial.print(operatingMode == OperatingMode::DEBUG ? F("debug")
                                                       : F("program"));
    Serial.print(F(" trace="));
    Serial.println(traceEnabled ? F("on") : F("off"));
  } else if (command == F("INFO")) {
    Serial.print(F("@OK board=mega2560 model=AT28C256 size=32768 page=64 "
                   "baud=500000 topology="));
#if STANDALONE_EEPROM
    Serial.print(F("standalone mode="));
#else
    Serial.print(F("incircuit mode="));
#endif
    Serial.print(operatingMode == OperatingMode::DEBUG ? F("debug")
                                                       : F("program"));
    Serial.print(F(" sdp_learned="));
    Serial.print(softwareProtectionRequired ? 1 : 0);
    Serial.print(F(" trace="));
    Serial.println(traceEnabled ? F("on") : F("off"));
  } else if (command == F("MODE PROGRAM")) {
    enterProgramMode();
    printOk(F("mode=program cpu_reset=low cpu_be=low"));
  } else if (command == F("MODE DEBUG")) {
    enterDebugMode();
    printOk(F("mode=debug trace=off cpu_restarted=1"));
  } else if (command == F("TRACE ON")) {
    if (operatingMode != OperatingMode::DEBUG) {
      printError(F("requires_debug_mode"));
    } else {
      clearDebugQueue();
      traceEnabled = true;
      printOk(F("trace=on max_lines_per_second=200"));
    }
  } else if (command == F("TRACE OFF")) {
    traceEnabled = false;
    clearDebugQueue();
    printOk(F("trace=off"));
  } else if (command == F("RESET")) {
    if (operatingMode != OperatingMode::DEBUG) {
      printError(F("requires_debug_mode"));
    } else {
      pulseCpuReset();
      printOk(F("cpu_restarted=1"));
    }
  } else if (command.startsWith(F("READ "))) {
    handleReadCommand(command.c_str());
  } else if (command.startsWith(F("WRITE "))) {
    handleWriteCommand(command.c_str());
  } else {
    printError(F("unknown_command"));
  }
}

void serviceCommands() {
  while (Serial.available()) {
    const char value = static_cast<char>(Serial.read());
    if (value == '\n') {
      handleCommand(commandBuffer);
      commandBuffer = "";
    } else if (value != '\r' && commandBuffer.length() < 100) {
      commandBuffer += value;
    }
  }
}

void printBinary(uint16_t value, uint8_t width) {
  for (int8_t bit = width - 1; bit >= 0; --bit) {
    Serial.print((value & (uint16_t{1} << bit)) ? '1' : '0');
  }
}

void serviceDebugCapture() {
  if (operatingMode != OperatingMode::DEBUG || !traceEnabled) {
    return;
  }

  // Polling avoids a 1 MHz interrupt storm. At full CPU speed this is a
  // representative live trace, not a lossless capture of every bus cycle.
  const bool clockHigh = (PINE & _BV(PE4)) != 0;
  if (clockHigh && !previousClockHigh) {
    latestDebugSample.address = sampleSystemAddress();
    latestDebugSample.data = sampleData();
    latestDebugSample.read = (PINE & _BV(PE5)) ? 1U : 0U;
    debugSamplePending = true;  // New samples overwrite the previous sample.
  }
  previousClockHigh = clockHigh;
}

void serviceDebugOutput() {
  if (operatingMode != OperatingMode::DEBUG || !traceEnabled ||
      !debugSamplePending || Serial.availableForWrite() < 48) {
    return;
  }

  static uint32_t lastOutputUs = 0;
  const uint32_t now = micros();
  if (static_cast<uint32_t>(now - lastOutputUs) < DEBUG_OUTPUT_INTERVAL_US) {
    return;
  }
  lastOutputUs = now;

  const BusSample sample = latestDebugSample;
  debugSamplePending = false;
  printBinary(sample.address, 16);
  Serial.print(F("   "));
  printBinary(sample.data, 8);
  char output[24];
  snprintf(output, sizeof(output), "   %04X  %c  %02X", sample.address,
           sample.read ? 'r' : 'W', sample.data);
  Serial.println(output);
}

}  // namespace

void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.setTimeout(1000);
  commandBuffer.reserve(104);

  addressBusInput();
  dataBusInput();
  controlsInput();
  releaseCpuReset();
  releaseCpuBusEnable();

  pinMode(CLOCK_PIN, INPUT);
  pinMode(READ_WRITE_PIN, INPUT);

  operatingMode = OperatingMode::PROGRAM;
  enterDebugMode();
  Serial.println(F("@READY AT28C256-MEGA 3.0 mode=debug"));
}

void loop() {
  serviceCommands();
  serviceDebugCapture();
  serviceDebugOutput();
}
