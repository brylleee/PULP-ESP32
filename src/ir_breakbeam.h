#pragma once

#include <Arduino.h>
#include <esp_timer.h>

// IR break-beam: an always-on IR transmitter shines straight into the receiver.
// Receiver output is active-LOW (LOW = IR carrier detected). Paper in the gap
// blocks the beam and the output goes/stays HIGH.
const uint8_t IR_SENSOR_PIN = 9;      // IR RX (receiver output)
const uint8_t IR_TX_PIN = 10;         // IR TX (IR LED, driven with 38kHz carrier)
const uint32_t IR_CARRIER_HZ = 38000;
const uint8_t IR_TX_LEDC_CHANNEL = 0;
// NEC frames repeat every 108ms with a ~40ms idle gap between them, so the
// timeout has to ride over that gap.
const uint32_t IR_LOST_TIMEOUT_US = 120000; // no IR activity for 120ms => blocked

// The VS1838's AGC suppresses anything that doesn't look like remote-control
// data (continuous carrier, regular burst trains). So the TX loops a genuine
// NEC frame, which is exactly what the receiver is built to pass through.
const uint8_t IR_NEC_ADDRESS = 0x00;
const uint8_t IR_NEC_COMMAND = 0x45;
const uint32_t IR_NEC_FRAME_US = 108000;

// Mark/space durations in us, alternating starting with a mark (carrier ON).
// 2 header + 64 bit + 1 stop mark + 1 trailing gap = 68 entries.
uint32_t irTxSeq[68];
uint8_t irTxSeqLen = 0;

volatile uint32_t irLastSeenUs = 0;
esp_timer_handle_t irTxTimer;

void irTxCarrier(bool on) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(IR_TX_PIN, on ? 128 : 0);
#else
  ledcWrite(IR_TX_LEDC_CHANNEL, on ? 128 : 0);
#endif
}

void irBuildNecFrame() {
  uint32_t data = (uint32_t)IR_NEC_ADDRESS
                | ((uint32_t)(uint8_t)~IR_NEC_ADDRESS << 8)
                | ((uint32_t)IR_NEC_COMMAND << 16)
                | ((uint32_t)(uint8_t)~IR_NEC_COMMAND << 24);
  uint32_t total = 0;
  irTxSeqLen = 0;
  auto push = [&](uint32_t us) { irTxSeq[irTxSeqLen++] = us; total += us; };

  push(9000); push(4500);                       // header
  for (uint8_t i = 0; i < 32; i++) {            // LSB first
    push(562);
    push((data >> i) & 1 ? 1687 : 562);
  }
  push(562);                                    // stop mark
  push(IR_NEC_FRAME_US - total);                // idle until next frame
}

// Steps through irTxSeq, toggling the carrier and re-arming itself.
void irTxStep(void *arg) {
  static uint8_t idx = 0;
  irTxCarrier((idx & 1) == 0); // even index = mark
  esp_timer_start_once(irTxTimer, irTxSeq[idx]);
  if (++idx >= irTxSeqLen) idx = 0;
}

// Demodulating receivers (TSOP/VS1838) chop a continuous carrier into pulses
// because of their AGC, so record every LOW edge instead of trusting one sample.
void IRAM_ATTR irEdgeIsr() {
  irLastSeenUs = micros();
}

bool irBeamPresent() {
  if (digitalRead(IR_SENSOR_PIN) == LOW) return true;
  return (micros() - irLastSeenUs) < IR_LOST_TIMEOUT_US;
}

void irInit() {
  pinMode(IR_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(IR_SENSOR_PIN), irEdgeIsr, FALLING);

  // IR transmitter: 38kHz carrier (50% duty), keyed with a looping NEC frame
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(IR_TX_PIN, IR_CARRIER_HZ, 8);
#else
  ledcSetup(IR_TX_LEDC_CHANNEL, IR_CARRIER_HZ, 8);
  ledcAttachPin(IR_TX_PIN, IR_TX_LEDC_CHANNEL);
#endif
  irTxCarrier(false);
  irBuildNecFrame();

  const esp_timer_create_args_t irTxTimerArgs = {
    .callback = &irTxStep,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "irTx"
  };
  esp_timer_create(&irTxTimerArgs, &irTxTimer);
  esp_timer_start_once(irTxTimer, 1000); // kicks off irTxStep, which re-arms itself
}
