#include <Arduino.h>

enum MODE {STANDBY, READ, WRITE};
typedef enum {
  OK,
  E_RESET,
  E_CORRUPT,
  E_UNEXPECTED,
  E_UNKNOWN
} error;


const unsigned int MAX_PAYLOAD = 63;
const unsigned int DELAY_US = 10;

//EEPROM CONTROL LINES
const unsigned int EEPROM_WE = A0;
const unsigned int EEPROM_OE = A1;
const unsigned int EEPROM_CE = A3;

//shift register control
const unsigned int SHIFT_OE = A3;
const unsigned int SHIFT_SER = A4;
const unsigned int SHIFT_RCLK = 12;
const unsigned int SHIFT_SCLK = 11;
const unsigned int SHIFT_CLR = 13;

//led
const unsigned int LEDPIN = 10;

const unsigned int dataPins[] = {2, 3, 4, 5, 6 ,7 ,8 ,9};

MODE mode = STANDBY;
error errno = OK;

/*
int readMode();
loadShiftAddr(unsigned int addr);
byte readAddr(unsigned int addr);
pulse(int pin);
*/


void setup() {
  Serial.begin(115200);
  Serial.setTimeout(120000L);

  pinMode(EEPROM_CE, OUTPUT);
  pinMode(EEPROM_OE, OUTPUT);
  pinMode(EEPROM_WE, OUTPUT);

  pinMode(SHIFT_OE, OUTPUT);
  pinMode(SHIFT_SER, OUTPUT);
  pinMode(SHIFT_RCLK, OUTPUT);
  pinMode(SHIFT_SCLK, OUTPUT);
  pinMode(SHIFT_CLR, OUTPUT);

  digitalWrite(SHIFT_OE, LOW);
  digitalWrite(SHIFT_CLR, HIGH);

  pinMode(LEDPIN, OUTPUT);
  digitalWrite(LEDPIN, LOW);

}

int readMode(){



}


void loop() {

//Leave empty for now

}


