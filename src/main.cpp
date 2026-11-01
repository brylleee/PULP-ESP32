#include "Keypad.h"
#include "Wire.h"
#include "LiquidCrystal_I2C.h"

#include "nmea-parser.h"

LiquidCrystal_I2C lcd(0x27, 16, 4);

const byte rows = 4;
const byte columns = 4;

byte rowpins[rows] = {15, 16, 17, 18};
byte columnpins[columns] = {4, 5, 6, 7};

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
const uint32_t FEEDER_POLL_MS = 20;

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

char keys[rows][columns] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

Keypad keypad = Keypad(makeKeymap(keys), rowpins, columnpins, rows, columns);

QueueHandle_t keyQueue;
QueueHandle_t lcdQueue;
SemaphoreHandle_t scanTriggerSem;   // Given by Master on SCAN_TRIGGER_KEY, taken by Feeder
const char SCAN_TRIGGER_KEY = 'C';  // Temporary manual trigger until the IR logic is in

// Serial ports (ARDUINO_USB_CDC_ON_BOOT=1):
//   Serial  - native USB (OTG) port
//   Serial0 - UART0, GPIO43/44, hard-wired to the DevKitC "UART" USB bridge.
//             Kept for PC monitoring only: the bridge drives GPIO44, so a Pi
//             wired there can't be heard (its SCAN_ACK gets drowned out).
//   Serial1 - UART1 on its own pins, dedicated to the Raspberry Pi.
const uint8_t PI_UART_TX_PIN = 11;  // -> Pi RX (GPIO15)
const uint8_t PI_UART_RX_PIN = 12;  // <- Pi TX (GPIO14)

// Build the frame once (same message ID) and send it to the Pi and both
// monitoring ports.
template <typename... Args>
uint32_t sendToPi(const std::string& cmd, const Args&... args) {
  uint32_t id = 0;
  std::string frame = buildNmea0183(&id, cmd, args...);
  const uint8_t *data = reinterpret_cast<const uint8_t*>(frame.c_str());
  Serial1.write(data, frame.length());
  Serial.write(data, frame.length());
  Serial0.write(data, frame.length());
  return id;
}
TaskHandle_t masterTaskHandle = NULL;
TaskHandle_t feederTaskHandle = NULL;
TaskHandle_t pulperTaskHandle = NULL;

// Global States; Read by LCD, Written by Master)
char PININPUT[11];
char confirmPIN[11];
volatile byte pinIndex = 0;
volatile uint16_t pagesScanned = 0;  // Pages confirmed by the Pi this session (read by LCD)

bool SCAN_MODE = 0;

// What the Pi answered to a SCAN, passed from Master to Feeder as the task
// notification value.
enum PiReply : uint32_t {
  PI_REPLY_NONE = 0,
  PI_REPLY_ACK = 1,   // SCAN_ACK: page captured
  PI_REPLY_NACK = 2   // NACK: the Pi failed the page
};

enum SystemState {
  STATE_STANDBY,
  STATE_MENU,
  STATE_INPUT_PIN,
  STATE_CONFIRM_PIN,
  STATE_PROCESSING
};

enum LcdScreen {
  SCREEN_STANDBY,
  SCREEN_MENU,
  SCREEN_INPUT_PIN,
  SCREEN_CONFIRM_PIN,
  SCREEN_PIN_SUCCESS,
  SCREEN_PIN_ERR,
  SCREEN_SCAN_READY,   // Waiting for the first page
  SCREEN_SCANNING,     // Pi is capturing a page
  SCREEN_PAGE_DONE,    // Page captured: next page or finish
  SCREEN_SCAN_ERR,     // Pi failed the page: retry or finish
  SCREEN_SHREDDING,
  SCREEN_PULPING,
  SCREEN_DONE
};

SystemState state;

void sendLcd(LcdScreen screen);

// TASK 1: I2C LCD TASK
void LcdTask(void *pvParameters) {
  LcdScreen currentScreen;

  for (;;) {
    if (xQueueReceive(lcdQueue, &currentScreen, portMAX_DELAY)) {
      lcd.clear();
      switch (currentScreen) {
        case SCREEN_STANDBY:
          lcd.setCursor(2, 1); lcd.print("[=] PULP Status:");
          lcd.setCursor(4, 2); lcd.print("Standby");
          break;
        case SCREEN_MENU:
          lcd.setCursor(0, 0); lcd.print("Select mode:");
          lcd.setCursor(0, 1); lcd.print("[1] Scan & Shred");
          lcd.setCursor(0, 2); lcd.print("[2] Shred only");
          break;
        case SCREEN_INPUT_PIN:
          lcd.setCursor(0, 0); lcd.print("Input Your PIN:");
          lcd.setCursor(0, 1); lcd.print("PIN:");
          for (int i = 0; i < pinIndex; i++) {
            lcd.setCursor(4 + i, 1); lcd.print("*");
          }
          break;
        case SCREEN_CONFIRM_PIN:
          lcd.setCursor(0, 0); lcd.print("Confirm PIN:");
          lcd.setCursor(0, 1); lcd.print("PIN:");
          for (int i = 0; i < pinIndex; i++) {
            lcd.setCursor(4 + i, 1); lcd.print("*");
          }
          break;
        case SCREEN_PIN_SUCCESS:
          lcd.setCursor(1, 1); lcd.print("PIN Confirmed!");
          break;
        case SCREEN_PIN_ERR:
          lcd.setCursor(2, 1); lcd.print("PINs Don't Match");
          break;
        case SCREEN_SCAN_READY:
          lcd.setCursor(0, 0); lcd.print("Ready to scan");
          lcd.setCursor(0, 1); lcd.print("Insert paper");
          lcd.setCursor(0, 2); lcd.print("C: Scan page");
          lcd.setCursor(0, 3); lcd.print("D: Finish");
          break;
        case SCREEN_SCANNING:
          lcd.setCursor(0, 0); lcd.print("Scanning...");
          lcd.setCursor(0, 1); lcd.print("Page "); lcd.print(pagesScanned + 1);
          lcd.setCursor(0, 2); lcd.print("Please wait");
          break;
        case SCREEN_PAGE_DONE:
          lcd.setCursor(0, 0); lcd.print("Page "); lcd.print(pagesScanned); lcd.print(" scanned!");
          lcd.setCursor(0, 1); lcd.print("Insert next page");
          lcd.setCursor(0, 2); lcd.print("C: Scan next");
          lcd.setCursor(0, 3); lcd.print("D: Finish");
          break;
        case SCREEN_SCAN_ERR:
          lcd.setCursor(0, 0); lcd.print("Scan failed!");
          lcd.setCursor(0, 1); lcd.print("Pages so far: "); lcd.print(pagesScanned);
          lcd.setCursor(0, 2); lcd.print("C: Retry");
          lcd.setCursor(0, 3); lcd.print("D: Finish");
          break;
        case SCREEN_SHREDDING:
          lcd.setCursor(2, 1); lcd.print("[=] PULP Status:");
          lcd.setCursor(4, 2); lcd.print("Shredding");
          break;
        case SCREEN_PULPING:
          lcd.setCursor(2, 1); lcd.print("[=] PULP Status:");
          lcd.setCursor(4, 2); lcd.print("Pulping");
          break;
        case SCREEN_DONE:
          lcd.setCursor(2, 1); lcd.print("[=] PULP Status:");
          lcd.setCursor(6, 2); lcd.print("Done");
          break;
      }
    }
  }
}

// TASK 2: KEYPAD TASK
void KeypadTask(void *pvParameters) {
  char key;
  for (;;) {
    key = keypad.getKey();
    if (key) {
      xQueueSend(keyQueue, &key, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// TASK 3: INPUT FEEDER TASK
void FeederTask(void *pvParameters) {
  for(;;) {
    // Beam check: spam while blocked, stay silent while the beam is hitting.
    //if (!irBeamPresent()) {
    //  Serial.println("BLOCKED");
    //}

    // If we're actively taking in papers
    if(state == STATE_PROCESSING) {
      // Check if outer input feeder IR is blocked (paper in)
      // Then roll the roller pins

      // Stop if inner input feeder IR is blocked but not outer.
      // It means: paper is about to go fully inside

      // Take a picture
      if(SCAN_MODE) {
        // Turn on LED here...

        // -- WAIT CONFIRMATION HERE
        // Temporary: wait for SCAN_TRIGGER_KEY. Times out each poll so the task
        // notices if the session is cancelled/finished. Replace with IR logic.
        if (xSemaphoreTake(scanTriggerSem, pdMS_TO_TICKS(FEEDER_POLL_MS)) != pdTRUE) {
          continue;
        }

        // Drop any stale notification (e.g. the wake-up give from Master) so
        // the wait below really waits for this page's SCAN_ACK.
        xTaskNotifyWait(0, 0xFFFFFFFF, NULL, 0);

        sendToPi("SCAN");
        sendLcd(SCREEN_SCANNING);

        // Wait for SCAN_ACK (or NACK if the Pi failed the page). No timeout.
        // Only bail out if the session is ended (D / A) so the feeder isn't
        // left stuck waiting for a page that no longer matters.
        uint32_t reply = PI_REPLY_NONE;
        while (xTaskNotifyWait(0, 0xFFFFFFFF, &reply, pdMS_TO_TICKS(FEEDER_POLL_MS)) != pdTRUE) {
          if (state != STATE_PROCESSING || !SCAN_MODE) break;
        }

        // Only update the screen if the session is still running; after D / A
        // Master owns the LCD.
        if (state == STATE_PROCESSING && SCAN_MODE) {
          if (reply == PI_REPLY_ACK) {
            pagesScanned++;
            sendLcd(SCREEN_PAGE_DONE);
          } else if (reply == PI_REPLY_NACK) {
            sendLcd(SCREEN_SCAN_ERR);
          }
        }

        // Then move the conveyor belt one full cycle.
      }
    }

    vTaskDelay(pdMS_TO_TICKS(FEEDER_POLL_MS));
  }
}

// TASK 4: PULPER TASK
void PulperTask(void *pvParameters) {
  for (;;) {
    // Sleep and wait for Master Task to trigger pulping
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    vTaskDelay(pdMS_TO_TICKS(3000)); // Simulate pulping duration
  }
}

void sendLcd(LcdScreen screen) {
  xQueueSend(lcdQueue, &screen, portMAX_DELAY);
}

// Read one NMEA line from a Pi port, if any, and act on it.
void handlePiPort(Stream &port) {
  if (!port.available()) return;

  String command = port.readStringUntil('\n');
  NmeaNode *nmea = parseNmea0183(command.c_str());
  if (nmea == NULL) return;

  // Tells ESP32 an individual page scan is successful
  if (nmea->token == "SCAN_ACK") {
    xTaskNotify(feederTaskHandle, PI_REPLY_ACK, eSetValueWithOverwrite);
  }

  // The Pi refused this page: release the feeder so the user can retry
  // instead of leaving it blocked on a SCAN_ACK that will never come.
  else if (nmea->token == "NACK") {
    xTaskNotify(feederTaskHandle, PI_REPLY_NACK, eSetValueWithOverwrite);
  }

  freeNmeaList(nmea);
}

// TASK 5: MASTER ORCHESTRATION TASK
void MasterTask(void *pvParameters) {
  state = STATE_STANDBY;
  sendLcd(SCREEN_STANDBY);
  char key;

  for (;;) {
    // Process Keypad Queue
    if (xQueueReceive(keyQueue, &key, pdMS_TO_TICKS(10))) {

      if (key == 'A') { // Global cancel button
        SCAN_MODE = 0;
        state = STATE_STANDBY;
        sendLcd(SCREEN_STANDBY);
      }

      else switch (state) {

        case STATE_STANDBY:
          if (key == '#') {
            state = STATE_MENU;
            sendLcd(SCREEN_MENU);
          }
          break;

        case STATE_MENU:
          if (key == '1') { // Scan & Shred
            pinIndex = 0;
            memset(PININPUT, 0, sizeof(PININPUT));
            state = STATE_INPUT_PIN;
            sendLcd(SCREEN_INPUT_PIN);
            SCAN_MODE = 1;
          } else if (key == '2') { // Shred Only (Bypass Crypto)
            state = STATE_PROCESSING;
            sendLcd(SCREEN_SHREDDING);
            vTaskDelay(pdMS_TO_TICKS(2000));
            xTaskNotifyGive(pulperTaskHandle); // Trigger Pulper
            sendLcd(SCREEN_PULPING);
            vTaskDelay(pdMS_TO_TICKS(3500));
            state = STATE_STANDBY;
            sendLcd(SCREEN_STANDBY);
          }
          break;

        case STATE_INPUT_PIN:
          if (key >= '0' && key <= '9' && pinIndex < 10) {
            PININPUT[pinIndex++] = key;
            sendLcd(SCREEN_INPUT_PIN);

            if (pinIndex == 10) {
              PININPUT[10] = '\0';
              vTaskDelay(pdMS_TO_TICKS(500)); // Brief pause for UX
              pinIndex = 0;
              memset(confirmPIN, 0, sizeof(confirmPIN));
              state = STATE_CONFIRM_PIN;
              sendLcd(SCREEN_CONFIRM_PIN);
            }
          }
          break;

        case STATE_CONFIRM_PIN:
          if (key >= '0' && key <= '9' && pinIndex < 10) {
            confirmPIN[pinIndex++] = key;
            sendLcd(SCREEN_CONFIRM_PIN);

            if (pinIndex == 10) {
              confirmPIN[10] = '\0';
              if (strcmp(PININPUT, confirmPIN) == 0) {
                sendLcd(SCREEN_PIN_SUCCESS);
                // Transmit PIN to RPi over UART
                sendToPi("SCANMODE", PININPUT);

                // Zeroize PIN in RAM immediately after transmission
                memset(PININPUT, 0, sizeof(PININPUT));
                memset(confirmPIN, 0, sizeof(confirmPIN));

                vTaskDelay(pdMS_TO_TICKS(1500));
                pagesScanned = 0;
                state = STATE_PROCESSING;

                // Feeder now waits for C; tell the user what to do.
                sendLcd(SCREEN_SCAN_READY);

              } else {
                sendLcd(SCREEN_PIN_ERR);
                vTaskDelay(pdMS_TO_TICKS(1500));
                state = STATE_STANDBY;
                sendLcd(SCREEN_STANDBY);
              }
            }
          }
          break;

        case STATE_PROCESSING:
          // Temporary manual trigger: tell Feeder to scan the current page
          if (key == SCAN_TRIGGER_KEY && SCAN_MODE) {
            xSemaphoreGive(scanTriggerSem);
          }

          // User presses 'D' to indicate they are done feeding the batch of papers
          if (key == 'D') {
            if (SCAN_MODE) {
              // Tell the Raspberry Pi to close the file/session
              sendToPi("SCAN_DONE");
            }
            SCAN_MODE = 0;

            // Visual feedback
            sendLcd(SCREEN_DONE);
            vTaskDelay(pdMS_TO_TICKS(2000)); // Show "Done" for 2 seconds

            // Go back to sleep
            state = STATE_STANDBY;
            sendLcd(SCREEN_STANDBY);
          }
          break;
      }
    }

    // Serial comms from Raspberry Pi (Serial1). USB and the UART bridge are
    // also accepted so replies can be typed in from a PC while testing.
    handlePiPort(Serial1);
    handlePiPort(Serial);
    handlePiPort(Serial0);
  }
}

void setup() {
  Serial.begin(115200);
  Serial0.begin(115200);  // UART0 default pins 43/44 -> "UART" USB port (monitoring)
  Serial1.begin(115200, SERIAL_8N1, PI_UART_RX_PIN, PI_UART_TX_PIN);  // Pi link

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

  Wire.begin(1, 2);
  lcd.init();
  lcd.backlight();

  keyQueue = xQueueCreate(10, sizeof(char));
  lcdQueue = xQueueCreate(10, sizeof(LcdScreen));
  scanTriggerSem = xSemaphoreCreateBinary();

  xTaskCreatePinnedToCore(LcdTask, "LcdTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(KeypadTask, "KeypadTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(FeederTask, "FeederTask", 4096, NULL, 1, &feederTaskHandle, 1);
  xTaskCreatePinnedToCore(PulperTask, "PulperTask", 4096, NULL, 1, &pulperTaskHandle, 1);
  xTaskCreatePinnedToCore(MasterTask, "MasterTask", 4096, NULL, 2, &masterTaskHandle, 1);
}

void loop() { vTaskDelete(NULL); }