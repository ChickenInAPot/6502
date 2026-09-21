#include <Arduino.h>
#include <EEPROM.h>
#include <SPI.h>
#include <stdarg.h>
#include <stddef.h>

#include "config.h"

namespace {

// The Mega has 8 KiB of SRAM, so images are transferred one physical EEPROM
// page at a time instead of buffering the entire 32 KiB image.
uint8_t pageBuffer[EEPROM_PAGE_SIZE];

void serialPrintf(const char *format, ...) {
  char buffer[180];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(buffer, sizeof(buffer), format, arguments);
  va_end(arguments);
  Serial.print(buffer);
}

uint32_t crc32Update(uint32_t state, const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    state ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      state = (state >> 1) ^ (0xEDB88320UL & (0UL - (state & 1UL)));
    }
  }
  return state;
}

uint32_t crc32(const uint8_t *data, size_t length) {
  return ~crc32Update(0xFFFFFFFFUL, data, length);
}

class EepromBus {
 public:
  struct ProgramResult {
    bool ok;
    uint8_t changedBytes;
    bool pageWritten;
    bool softwareProtectionDetected;
    uint16_t failedAddress;
  };

  void begin() {
    // Pull control lines inactive before configuring the data bus or SPI.
    pinMode(EEPROM_CE_PIN, OUTPUT);
    pinMode(EEPROM_OE_PIN, OUTPUT);
    pinMode(EEPROM_WE_PIN, OUTPUT);
    pinMode(SHIFT_LATCH_PIN, OUTPUT);
    digitalWrite(EEPROM_CE_PIN, HIGH);
    digitalWrite(EEPROM_OE_PIN, HIGH);
    digitalWrite(EEPROM_WE_PIN, HIGH);
    digitalWrite(SHIFT_LATCH_PIN, LOW);
    setDataPinsInput();

    SPI.begin();
    SPI.beginTransaction(SPISettings(SHIFT_CLOCK_HZ, MSBFIRST, SPI_MODE0));
    setAddress(0);
  }

  void standby() {
    digitalWrite(EEPROM_OE_PIN, HIGH);
    digitalWrite(EEPROM_WE_PIN, HIGH);
    digitalWrite(EEPROM_CE_PIN, HIGH);
    setDataPinsInput();
  }

  uint8_t readByte(uint16_t address) {
    beginRead();
    const uint8_t value = readByteInReadMode(address);
    standby();
    return value;
  }

  void readRange(uint16_t start, uint8_t *destination, uint8_t length) {
    beginRead();
    for (uint8_t i = 0; i < length; ++i) {
      destination[i] = readByteInReadMode(start + i);
    }
    standby();
  }

  // Program one sequential chunk that never crosses a 64-byte page boundary.
  // protectedProgramming is retained across chunks in the same upload.
  ProgramResult programChunk(uint16_t start, const uint8_t *source,
                             uint8_t length, bool &protectedProgramming) {
    ProgramResult result{true, 0, false, false, 0};
    const uint16_t pageStart = start & ~(EEPROM_PAGE_SIZE - 1);
    uint64_t dirtyMask = 0;

    beginRead();
    for (uint8_t i = 0; i < length; ++i) {
      const uint16_t address = start + i;
      if (readByteInReadMode(address) != source[i]) {
        dirtyMask |= (uint64_t{1} << (address - pageStart));
        ++result.changedBytes;
      }
    }
    standby();

    if (dirtyMask == 0) {
      return result;
    }

    bool pageOk = programPage(pageStart, start, source, dirtyMask,
                              protectedProgramming);
    if (!pageOk && !protectedProgramming) {
      // Software-protected chips ignore an ordinary write. A verification
      // failure triggers one retry using Microchip's AA/55/A0 sequence.
      pageOk = programPage(pageStart, start, source, dirtyMask, true);
      if (pageOk) {
        protectedProgramming = true;
        result.softwareProtectionDetected = true;
      }
    }

    if (!pageOk) {
      result.ok = false;
      for (uint8_t offset = 0; offset < EEPROM_PAGE_SIZE; ++offset) {
        if (dirtyMask & (uint64_t{1} << offset)) {
          result.failedAddress = pageStart + offset;
          break;
        }
      }
      standby();
      return result;
    }

    result.pageWritten = true;
    return result;
  }

 private:
  void setAddress(uint16_t address) {
    // High byte first: after 16 clocks the near 595 contains A0..A7 and the
    // far 595 contains A8..A14.
    digitalWrite(SHIFT_LATCH_PIN, LOW);
    SPI.transfer(static_cast<uint8_t>(address >> 8));
    SPI.transfer(static_cast<uint8_t>(address));
    digitalWrite(SHIFT_LATCH_PIN, HIGH);
    digitalWrite(SHIFT_LATCH_PIN, LOW);
  }

  // Mega pins 22..29 are PA0..PA7, so the complete data bus is one register.
  void setDataPinsInput() {
    DDRA = 0x00;
    PORTA = 0x00;  // input with internal pull-ups disabled
  }

  void setDataPinsOutput() { DDRA = 0xFF; }

  void writeData(uint8_t value) { PORTA = value; }

  uint8_t sampleData() const { return PINA; }

  void beginRead() {
    digitalWrite(EEPROM_CE_PIN, HIGH);
    digitalWrite(EEPROM_OE_PIN, HIGH);
    digitalWrite(EEPROM_WE_PIN, HIGH);
    setDataPinsInput();
    digitalWrite(EEPROM_CE_PIN, LOW);
    digitalWrite(EEPROM_OE_PIN, LOW);
  }

  uint8_t readByteInReadMode(uint16_t address) {
    setAddress(address);
    delayMicroseconds(1);  // AT28C256 access time is at most 150 ns
    return sampleData();
  }

  void beginWrite() {
    digitalWrite(EEPROM_CE_PIN, HIGH);
    digitalWrite(EEPROM_OE_PIN, HIGH);  // EEPROM releases I/O0..I/O7
    digitalWrite(EEPROM_WE_PIN, HIGH);
    setDataPinsOutput();
    digitalWrite(EEPROM_CE_PIN, LOW);
  }

  void loadWriteByte(uint16_t address, uint8_t value) {
    setAddress(address);
    writeData(value);
    delayMicroseconds(1);  // exceeds 50 ns address/data setup requirement
    digitalWrite(EEPROM_WE_PIN, LOW);
    delayMicroseconds(1);  // exceeds 100 ns minimum write pulse
    digitalWrite(EEPROM_WE_PIN, HIGH);
  }

  bool waitForWrite(uint16_t lastAddress, uint8_t lastValue) {
    // Allow the maximum 150 us page-load window to close before polling I/O7.
    delayMicroseconds(200);
    digitalWrite(EEPROM_CE_PIN, HIGH);
    setDataPinsInput();
    setAddress(lastAddress);
    digitalWrite(EEPROM_CE_PIN, LOW);

    const uint32_t started = micros();
    while (static_cast<uint32_t>(micros() - started) < WRITE_TIMEOUT_US) {
      digitalWrite(EEPROM_OE_PIN, LOW);
      delayMicroseconds(1);
      const uint8_t value = sampleData();
      digitalWrite(EEPROM_OE_PIN, HIGH);
      delayMicroseconds(1);
      if ((value & 0x80u) == (lastValue & 0x80u)) {
        standby();
        return true;
      }
    }
    standby();
    return false;
  }

  bool verifyPage(uint16_t pageStart, uint16_t sourceStart,
                  const uint8_t *source, uint64_t dirtyMask) {
    beginRead();
    for (uint8_t offset = 0; offset < EEPROM_PAGE_SIZE; ++offset) {
      if ((dirtyMask & (uint64_t{1} << offset)) == 0) {
        continue;
      }
      const uint16_t address = pageStart + offset;
      if (readByteInReadMode(address) != source[address - sourceStart]) {
        standby();
        return false;
      }
    }
    standby();
    return true;
  }

  bool programPage(uint16_t pageStart, uint16_t sourceStart,
                   const uint8_t *source, uint64_t dirtyMask,
                   bool protectedProgramming) {
    beginWrite();
    if (protectedProgramming) {
      loadWriteByte(0x5555, 0xAA);
      loadWriteByte(0x2AAA, 0x55);
      loadWriteByte(0x5555, 0xA0);
    }

    uint16_t lastAddress = pageStart;
    uint8_t lastValue = 0;
    for (uint8_t offset = 0; offset < EEPROM_PAGE_SIZE; ++offset) {
      if ((dirtyMask & (uint64_t{1} << offset)) == 0) {
        continue;
      }
      lastAddress = pageStart + offset;
      lastValue = source[lastAddress - sourceStart];
      loadWriteByte(lastAddress, lastValue);
    }

    if (!waitForWrite(lastAddress, lastValue)) {
      return false;
    }
    return verifyPage(pageStart, sourceStart, source, dirtyMask);
  }
};

EepromBus eeprom;

struct __attribute__((packed)) CacheRecord {
  uint32_t magic;
  uint16_t start;
  uint16_t length;
  uint32_t imageCrc;
  uint32_t recordCrc;
};

constexpr uint32_t CACHE_MAGIC = 0x41543238UL;  // ASCII "AT28"

bool loadCache(CacheRecord &record) {
  EEPROM.get(0, record);
  const uint32_t expected = crc32(
      reinterpret_cast<const uint8_t *>(&record), offsetof(CacheRecord, recordCrc));
  return record.magic == CACHE_MAGIC && record.recordCrc == expected;
}

bool cacheMatches(uint16_t start, uint16_t length, uint32_t imageCrc) {
  CacheRecord record{};
  return loadCache(record) && record.start == start && record.length == length &&
         record.imageCrc == imageCrc;
}

void rememberUpload(uint16_t start, uint16_t length, uint32_t imageCrc) {
  if (cacheMatches(start, length, imageCrc)) {
    return;  // avoid needless writes to the Mega's internal EEPROM
  }
  CacheRecord record{CACHE_MAGIC, start, length, imageCrc, 0};
  record.recordCrc = crc32(reinterpret_cast<const uint8_t *>(&record),
                           offsetof(CacheRecord, recordCrc));
  EEPROM.put(0, record);  // put() updates only bytes that actually differ
}

bool receiveExact(uint8_t *destination, size_t length, uint32_t timeoutMs) {
  const uint32_t started = millis();
  size_t received = 0;
  while (received < length) {
    const int available = Serial.available();
    if (available > 0) {
      const size_t remaining = length - received;
      const size_t chunk = static_cast<size_t>(available) < remaining
                               ? static_cast<size_t>(available)
                               : remaining;
      received += Serial.readBytes(destination + received, chunk);
    } else if (static_cast<uint32_t>(millis() - started) >= timeoutMs) {
      return false;
    }
  }
  return true;
}

bool readLineWithTimeout(String &line, uint32_t timeoutMs) {
  const uint32_t started = millis();
  line = "";
  while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
    while (Serial.available()) {
      const char value = static_cast<char>(Serial.read());
      if (value == '\n') {
        line.trim();
        return true;
      }
      if (value != '\r' && line.length() < 95) {
        line += value;
      }
    }
  }
  return false;
}

void printInfo() {
  CacheRecord record{};
  if (loadCache(record)) {
    serialPrintf(
        "OK board=mega2560 model=AT28C256 size=%u page=%u cache_valid=1 "
        "last_start=%u last_length=%u last_crc32=%08lX\n",
        EEPROM_SIZE, EEPROM_PAGE_SIZE, record.start, record.length,
        static_cast<unsigned long>(record.imageCrc));
  } else {
    serialPrintf(
        "OK board=mega2560 model=AT28C256 size=%u page=%u cache_valid=0\n",
        EEPROM_SIZE, EEPROM_PAGE_SIZE);
  }
}

void handleWrite(const char *line) {
  unsigned long startValue = 0;
  unsigned long lengthValue = 0;
  unsigned long expectedCrc = 0;
  char option[12] = "";
  const int fields = sscanf(line, "WRITE %lu %lu %lx %11s", &startValue,
                            &lengthValue, &expectedCrc, option);
  if (fields < 3 || lengthValue == 0 || startValue >= EEPROM_SIZE ||
      startValue + lengthValue > EEPROM_SIZE) {
    Serial.println(F("ERR invalid_write_range"));
    return;
  }

  const bool trustCache = fields == 4 && strcmp(option, "TRUST") == 0;
  if (fields == 4 && !trustCache) {
    Serial.println(F("ERR invalid_write_option"));
    return;
  }
  if (trustCache && cacheMatches(startValue, lengthValue, expectedCrc)) {
    serialPrintf(
        "OK received=0 changed=0 pages=0 cache_hit=1 crc32=%08lX "
        "elapsed_ms=0\n",
        expectedCrc);
    return;
  }

  Serial.println(F("READY"));
  const uint32_t started = millis();
  uint32_t rollingCrc = 0xFFFFFFFFUL;
  uint32_t received = 0;
  uint32_t changed = 0;
  uint16_t pages = 0;
  bool protectedProgramming = false;
  bool protectionDetected = false;

  while (received < lengthValue) {
    String pageLine;
    if (!readLineWithTimeout(pageLine, 5000)) {
      Serial.println(F("ERR page_header_timeout"));
      return;
    }

    unsigned long pageAddress = 0;
    unsigned int pageLength = 0;
    unsigned long pageCrc = 0;
    if (sscanf(pageLine.c_str(), "PAGE %lu %u %lx", &pageAddress, &pageLength,
               &pageCrc) != 3 ||
        pageAddress != startValue + received || pageLength == 0 ||
        pageLength > EEPROM_PAGE_SIZE || pageLength > lengthValue - received ||
        (pageAddress / EEPROM_PAGE_SIZE) !=
            ((pageAddress + pageLength - 1) / EEPROM_PAGE_SIZE)) {
      Serial.println(F("ERR invalid_page_frame"));
      return;
    }

    Serial.println(F("SEND"));
    if (!receiveExact(pageBuffer, pageLength, 3000)) {
      Serial.println(F("ERR page_data_timeout"));
      return;
    }
    const uint32_t actualPageCrc = crc32(pageBuffer, pageLength);
    if (actualPageCrc != static_cast<uint32_t>(pageCrc)) {
      serialPrintf("ERR page_crc expected=%08lX actual=%08lX\n", pageCrc,
                   static_cast<unsigned long>(actualPageCrc));
      return;
    }

    rollingCrc = crc32Update(rollingCrc, pageBuffer, pageLength);
    const auto result = eeprom.programChunk(
        static_cast<uint16_t>(pageAddress), pageBuffer,
        static_cast<uint8_t>(pageLength), protectedProgramming);
    if (!result.ok) {
      serialPrintf("ERR verify_failed address=0x%04X elapsed_ms=%lu\n",
                   result.failedAddress,
                   static_cast<unsigned long>(millis() - started));
      return;
    }

    changed += result.changedBytes;
    pages += result.pageWritten ? 1 : 0;
    protectionDetected |= result.softwareProtectionDetected;
    received += pageLength;
    serialPrintf("ACK received=%lu changed=%lu pages=%u\n",
                 static_cast<unsigned long>(received),
                 static_cast<unsigned long>(changed), pages);
  }

  const uint32_t actualCrc = ~rollingCrc;
  if (actualCrc != static_cast<uint32_t>(expectedCrc)) {
    serialPrintf("ERR crc_mismatch expected=%08lX actual=%08lX\n", expectedCrc,
                 static_cast<unsigned long>(actualCrc));
    return;
  }

  rememberUpload(static_cast<uint16_t>(startValue),
                 static_cast<uint16_t>(lengthValue), actualCrc);
  serialPrintf(
      "OK received=%lu changed=%lu pages=%u cache_hit=0 sdp=%u "
      "crc32=%08lX elapsed_ms=%lu\n",
      static_cast<unsigned long>(received), static_cast<unsigned long>(changed),
      pages, protectionDetected ? 1 : 0, static_cast<unsigned long>(actualCrc),
      static_cast<unsigned long>(millis() - started));
}

uint32_t crcRange(uint16_t start, uint16_t length) {
  uint32_t state = 0xFFFFFFFFUL;
  uint32_t offset = 0;
  while (offset < length) {
    const uint8_t count =
        (length - offset) > EEPROM_PAGE_SIZE
            ? EEPROM_PAGE_SIZE
            : static_cast<uint8_t>(length - offset);
    eeprom.readRange(start + offset, pageBuffer, count);
    state = crc32Update(state, pageBuffer, count);
    offset += count;
  }
  return ~state;
}

void handleRead(const char *line) {
  unsigned long startValue = 0;
  unsigned long lengthValue = 0;
  if (sscanf(line, "READ %lu %lu", &startValue, &lengthValue) != 2 ||
      lengthValue == 0 || startValue >= EEPROM_SIZE ||
      startValue + lengthValue > EEPROM_SIZE) {
    Serial.println(F("ERR invalid_read_range"));
    return;
  }

  const uint32_t crc = crcRange(startValue, lengthValue);
  serialPrintf("DATA %lu %08lX\n", lengthValue,
               static_cast<unsigned long>(crc));

  uint32_t sent = 0;
  while (sent < lengthValue) {
    const uint8_t count =
        (lengthValue - sent) > EEPROM_PAGE_SIZE
            ? EEPROM_PAGE_SIZE
            : static_cast<uint8_t>(lengthValue - sent);
    eeprom.readRange(startValue + sent, pageBuffer, count);
    Serial.write(pageBuffer, count);
    sent += count;
  }
  Serial.print(F("\nOK\n"));
}

void handleCommand(const String &command) {
  if (command == F("PING")) {
    Serial.println(F("OK firmware=2.0 protocol=2"));
  } else if (command == F("INFO")) {
    printInfo();
  } else if (command.startsWith(F("WRITE "))) {
    handleWrite(command.c_str());
  } else if (command.startsWith(F("READ "))) {
    handleRead(command.c_str());
  } else {
    Serial.println(F("ERR unknown_command"));
  }
}

}  // namespace

void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.setTimeout(1000);
  eeprom.begin();
  delay(50);
  Serial.println(F("AT28C256-MEGA-PROGRAMMER 2.0 READY"));
}

void loop() {
  if (!Serial.available()) {
    return;
  }
  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command.length() > 0) {
    handleCommand(command);
  }
}
