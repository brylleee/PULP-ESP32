#pragma once

// Pins and raw I/O. No timing or state logic here; use the actions in
// main.cpp instead of calling these directly. Include it from main.cpp only.

#include <Arduino.h>
#include <ESP32Servo.h>

// Pi UART
constexpr uint8_t PI_UART_TX_PIN = 11;  // -> Pi RX (GPIO15)
constexpr uint8_t PI_UART_RX_PIN = 12;  // <- Pi TX (GPIO14)

// Scan flash, HIGH = on
constexpr uint8_t SCAN_FLASH_PIN = 13;

// Conveyor
constexpr uint8_t ENTRANCE_IR_PIN = 47;   // TCRT5000, LOW = paper
constexpr uint8_t CONVEYOR_ENA_PIN = 41;  // motor driver ENA (PWM)

// Shredder relay, HIGH = on
constexpr uint8_t SHREDDER_RELAY_PIN = 38;  // was 42 (dead)

// Pulper
constexpr uint8_t PULPER_RELAY_PIN = 14;  // blender relay
constexpr uint8_t PULPER_SERVO_PIN = 21;  // door servo
constexpr uint8_t WATER_PUMP_PIN = 39;
constexpr uint8_t WATER_LEVEL_PIN = 8;    // level of the container the pump draws from
// true: analog strip module (ADC, higher = more water)
// false: float switch to GND (LOW = water at the switch)
constexpr bool WATER_SENSOR_ANALOG = true;

constexpr uint8_t DOOR_CLOSED_ANGLE = 0;
constexpr uint8_t DOOR_OPEN_ANGLE = 90;

static Servo doorServo;

// Output pins read back as 0, so track them here
static volatile bool hwFlashState = false;
static volatile uint8_t hwDutyState = 0;
static volatile bool hwShredderState = false;
static volatile uint8_t hwDoorState = DOOR_CLOSED_ANGLE;
static volatile bool hwPumpState = false;
static volatile bool hwBlenderState = false;

static inline void hwFlash(bool on) {
  hwFlashState = on;
  digitalWrite(SCAN_FLASH_PIN, on ? HIGH : LOW);
}

static inline bool hwFlashIsOn() { return hwFlashState; }

static inline void hwConveyorDuty(uint8_t duty) {  // 0-255
  hwDutyState = duty;
  analogWrite(CONVEYOR_ENA_PIN, duty);
}

static inline uint8_t hwConveyorDutyNow() { return hwDutyState; }

static inline void hwShredderRelay(bool on) {
  hwShredderState = on;
  digitalWrite(SHREDDER_RELAY_PIN, on ? HIGH : LOW);
}

static inline bool hwShredderRelayIsOn() { return hwShredderState; }

static inline bool hwPaperAtEntrance() { return digitalRead(ENTRANCE_IR_PIN) == LOW; }

// 0-4095 (a float switch reads 0 or 4095)
static inline uint16_t hwWaterRaw() {
  if (WATER_SENSOR_ANALOG) return analogRead(WATER_LEVEL_PIN);
  return digitalRead(WATER_LEVEL_PIN) == LOW ? 4095 : 0;
}

static inline void hwDoorAngle(uint8_t degrees) {
  hwDoorState = degrees;
  doorServo.write(degrees);
}

static inline uint8_t hwDoorAngleNow() { return hwDoorState; }

static inline void hwPump(bool on) {
  hwPumpState = on;
  digitalWrite(WATER_PUMP_PIN, on ? HIGH : LOW);
}

static inline bool hwPumpIsOn() { return hwPumpState; }

static inline void hwBlender(bool on) {
  hwBlenderState = on;
  digitalWrite(PULPER_RELAY_PIN, on ? HIGH : LOW);
}

static inline bool hwBlenderIsOn() { return hwBlenderState; }

// All outputs off, door closed
static inline void hardwareInit() {
  pinMode(SCAN_FLASH_PIN, OUTPUT);
  hwFlash(false);

  pinMode(ENTRANCE_IR_PIN, INPUT);
  pinMode(CONVEYOR_ENA_PIN, OUTPUT);
  hwConveyorDuty(0);

  pinMode(SHREDDER_RELAY_PIN, OUTPUT);
  hwShredderRelay(false);

  pinMode(PULPER_RELAY_PIN, OUTPUT);
  hwBlender(false);
  pinMode(WATER_PUMP_PIN, OUTPUT);
  hwPump(false);
  if (WATER_SENSOR_ANALOG) {
    analogReadResolution(12);
    analogSetPinAttenuation(WATER_LEVEL_PIN, ADC_11db);  // full 0-3.3 V range
  } else {
    pinMode(WATER_LEVEL_PIN, INPUT_PULLUP);
  }
  // attach() starts at 90 deg, so the door may twitch at boot
  ESP32PWM::allocateTimer(1);  // timer 0 is the IR carrier
  doorServo.setPeriodHertz(50);
  doorServo.attach(PULPER_SERVO_PIN, 500, 2500);  // us for 0-180 deg
  hwDoorAngle(DOOR_CLOSED_ANGLE);
}
