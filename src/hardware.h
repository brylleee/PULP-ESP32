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
// On/off water sensor at the very bottom of the container the pump draws
// from: a 3-wire module on 5 V, NPN open-collector output. It pulls the pin LOW
// while it detects water and lets go when the container is empty, so the pin
// is pulled up (to 3.3 V: safe with a 5 V module). A loose wire reads HIGH,
// "no water", and the pump stays off. The Pi's WATER_ACTIVE flips the polarity
// for a module that works the other way round (see waterSetActiveLevel()).
constexpr uint8_t WATER_SENSOR_PIN = 8;

// Pi wake: pulled LOW for a moment to start a halted Pi. Wire it to the Pi's
// GPIO3 (header pin 5); harmless when not wired. Open drain: never driven HIGH.
constexpr uint8_t PI_WAKE_PIN = 40;
constexpr uint32_t PI_WAKE_PULSE_MS = 300;

constexpr uint8_t DOOR_CLOSED_ANGLE = 0;
constexpr uint8_t DOOR_OPEN_ANGLE = 90;

static Servo doorServo;

// Output pins read back as 0, so track them here
static volatile bool hwFlashState = false;
static volatile uint8_t hwDutyState = 0;
static volatile bool hwShredderState = false;
static volatile uint8_t hwDoorState = DOOR_OPEN_ANGLE;
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

static inline bool hwWaterPinHigh() { return digitalRead(WATER_SENSOR_PIN) == HIGH; }

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

static inline void hwPiWakePulse() {
  digitalWrite(PI_WAKE_PIN, LOW);
  pinMode(PI_WAKE_PIN, OUTPUT);
  delay(PI_WAKE_PULSE_MS);
  pinMode(PI_WAKE_PIN, INPUT);
}

// Everything that moves or lights up off, at once
static inline void hwAllOff() {
  hwConveyorDuty(0);
  hwShredderRelay(false);
  hwPump(false);
  hwBlender(false);
  hwFlash(false);
}

// All outputs off, door open (its resting position)
static inline void hardwareInit() {
  pinMode(SCAN_FLASH_PIN, OUTPUT);
  hwFlash(false);

  pinMode(ENTRANCE_IR_PIN, INPUT);
  pinMode(CONVEYOR_ENA_PIN, OUTPUT);
  hwConveyorDuty(0);

  pinMode(SHREDDER_RELAY_PIN, OUTPUT);
  hwShredderRelay(false);

  pinMode(PI_WAKE_PIN, INPUT);  // released

  pinMode(PULPER_RELAY_PIN, OUTPUT);
  hwBlender(false);
  pinMode(WATER_PUMP_PIN, OUTPUT);
  hwPump(false);
  pinMode(WATER_SENSOR_PIN, INPUT_PULLUP);
  // attach() starts at 90 deg (open), the door's resting position
  ESP32PWM::allocateTimer(1);  // timer 0 is the IR carrier
  doorServo.setPeriodHertz(50);
  doorServo.attach(PULPER_SERVO_PIN, 500, 2500);  // us for 0-180 deg
  hwDoorAngle(DOOR_OPEN_ANGLE);
}
