#include "Keypad.h"
#include "Wire.h"
#include "LiquidCrystal_I2C.h"

// #include <ESP32Servo.h>  // Door servo; also uncomment lib_deps in platformio.ini

#include "nmea-parser.h"

#include "ir_breakbeam.h"

LiquidCrystal_I2C lcd(0x27, 16, 2);

const byte rows = 4;
const byte columns = 4;

byte rowpins[rows] = {15, 16, 17, 18};
byte columnpins[columns] = {4, 5, 6, 7};

const uint32_t FEEDER_POLL_MS = 20;

char keys[rows][columns] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

Keypad keypad = Keypad(makeKeymap(keys), rowpins, columnpins, rows, columns);

QueueHandle_t keyQueue;
QueueHandle_t lcdQueue;
SemaphoreHandle_t scanTriggerSem;   // Master gives on SCAN_TRIGGER_KEY, Feeder takes
const char SCAN_TRIGGER_KEY = 'C';  // Temporary manual trigger until IR logic is in

// Serial  - native USB
// Serial0 - GPIO43/44, wired to the DevKitC "UART" USB bridge: PC monitoring
//           only (the bridge drives GPIO44, drowning out a Pi wired there)
// Serial1 - Raspberry Pi link
const uint8_t PI_UART_TX_PIN = 11;  // -> Pi RX (GPIO15)
const uint8_t PI_UART_RX_PIN = 12;  // <- Pi TX (GPIO14)

const uint8_t SCAN_FLASH_PIN = 13;  // OUT: white LED flash transistor, HIGH = on during scan session

// Conveyor (scan mode). Paper at the entrance starts the belt; the Pi decides
// when to stop or adjust it over the NMEA link (CONV_STOP / CONV_RUN / CONV_SPEED).
const uint8_t ENTRANCE_IR_PIN = 40;    // IN:  TCRT5000 OUT, LOW = paper at feeder entrance
const uint8_t CONVEYOR_ENA_PIN = 41;   // OUT: motor driver ENA, PWM duty = belt speed
const uint8_t CONVEYOR_DEFAULT_SPEED = 100;     // analogWrite duty (0-255)
const uint32_t CONVEYOR_TIMEOUT_MS = 10000;     // Safety stop if the Pi never says stop
const uint8_t ENTRANCE_DEBOUNCE_POLLS = 3;      // Consecutive LOW reads before paper counts as present

// Pulper pins (not used yet)
const uint8_t PULPER_RELAY_PIN = 14;  // OUT: relay module, HIGH = pulper on
const uint8_t PULPER_SERVO_PIN = 21;  // OUT: door servo signal
const uint8_t WATER_LEVEL_PIN = 8;    // IN:  level sensor, INPUT_PULLUP (switch to GND)
const uint8_t WATER_PUMP_PIN = 39;    // OUT: pump MOSFET/relay, HIGH = on

SemaphoreHandle_t piTxMutex;         // Several tasks send to the Pi; keep frames whole
QueueHandle_t conveyorCmdQueue;      // Pi conveyor commands (duty 0-255, 0 = stop) -> ConveyorTask

// Send one frame (same message ID) to the Pi and both monitoring ports.
template <typename... Args>
uint32_t sendToPi(const std::string& cmd, const Args&... args) {
  uint32_t id = 0;
  std::string frame = buildNmea0183(&id, cmd, args...);
  const uint8_t *data = reinterpret_cast<const uint8_t*>(frame.c_str());
  xSemaphoreTake(piTxMutex, portMAX_DELAY);
  Serial1.write(data, frame.length());
  Serial.write(data, frame.length());
  Serial0.write(data, frame.length());
  xSemaphoreGive(piTxMutex);
  return id;
}
TaskHandle_t masterTaskHandle = NULL;
TaskHandle_t feederTaskHandle = NULL;
TaskHandle_t pulperTaskHandle = NULL;

// Global state: written by Master, read by LCD
char PININPUT[11];
char confirmPIN[11];
volatile byte pinIndex = 0;
volatile uint16_t pagesScanned = 0;  // Pages the Pi confirmed this session

bool SCAN_MODE = 0;

// Pi's reply to SCAN, sent to Feeder as a task notification value.
enum PiReply : uint32_t {
  PI_REPLY_NONE = 0,
  PI_REPLY_ACK = 1,   // SCAN_ACK: page captured
  PI_REPLY_NACK = 2   // NACK: page failed
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
  SCREEN_PAGE_DONE,    // Page captured: next or finish
  SCREEN_SCAN_ERR,     // Page failed: retry or finish
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
      // 16x2 display: rows 0-1, max 16 chars per row
      switch (currentScreen) {
        case SCREEN_STANDBY:
          lcd.setCursor(0, 0); lcd.print("[=] PULP Status:");
          lcd.setCursor(4, 1); lcd.print("Standby");
          break;
        case SCREEN_MENU:
          lcd.setCursor(0, 0); lcd.print("[1] Scan & Shred");
          lcd.setCursor(0, 1); lcd.print("[2] Shred only");
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
          lcd.setCursor(1, 0); lcd.print("PIN Confirmed!");
          break;
        case SCREEN_PIN_ERR:
          lcd.setCursor(0, 0); lcd.print("PINs Don't Match");
          break;
        case SCREEN_SCAN_READY:
          lcd.setCursor(0, 0); lcd.print("Insert paper");
          lcd.setCursor(0, 1); lcd.print("C:Scan D:Finish");
          break;
        case SCREEN_SCANNING:
          lcd.setCursor(0, 0); lcd.print("Scanning page "); lcd.print(pagesScanned + 1);
          lcd.setCursor(0, 1); lcd.print("Please wait...");
          break;
        case SCREEN_PAGE_DONE:
          lcd.setCursor(0, 0); lcd.print("Page "); lcd.print(pagesScanned); lcd.print(" scanned!");
          lcd.setCursor(0, 1); lcd.print("C:Next D:Finish");
          break;
        case SCREEN_SCAN_ERR:
          lcd.setCursor(0, 0); lcd.print("Failed. Pages:"); lcd.print(pagesScanned);
          lcd.setCursor(0, 1); lcd.print("C:Retry D:Finish");
          break;
        case SCREEN_SHREDDING:
          lcd.setCursor(0, 0); lcd.print("[=] PULP Status:");
          lcd.setCursor(4, 1); lcd.print("Shredding");
          break;
        case SCREEN_PULPING:
          lcd.setCursor(0, 0); lcd.print("[=] PULP Status:");
          lcd.setCursor(4, 1); lcd.print("Pulping");
          break;
        case SCREEN_DONE:
          lcd.setCursor(0, 0); lcd.print("[=] PULP Status:");
          lcd.setCursor(6, 1); lcd.print("Done");
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
      Serial.println(key);
      xQueueSend(keyQueue, &key, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// TASK 3: INPUT FEEDER TASK
void FeederTask(void *pvParameters) {
  for(;;) {
    // Flash on for the whole scan session, off within one poll after D / A.
    digitalWrite(SCAN_FLASH_PIN, (state == STATE_PROCESSING && SCAN_MODE) ? HIGH : LOW);

    // Beam check: print while blocked.
    //if (!irBeamPresent()) {
    //  Serial.println("BLOCKED");
    //}

    if(state == STATE_PROCESSING) {
      // Belt movement is handled by ConveyorTask.

      if(SCAN_MODE) {
        // Temporary: wait for SCAN_TRIGGER_KEY (replace with IR logic).
        // Short timeout so a cancelled/finished session is noticed.
        if (xSemaphoreTake(scanTriggerSem, pdMS_TO_TICKS(FEEDER_POLL_MS)) != pdTRUE) {
          continue;
        }

        // Clear stale notifications so we wait for this page's reply.
        xTaskNotifyWait(0, 0xFFFFFFFF, NULL, 0);

        sendToPi("SCAN");
        sendLcd(SCREEN_SCANNING);

        // Wait for SCAN_ACK / NACK, no timeout; bail only if the session ends.
        uint32_t reply = PI_REPLY_NONE;
        while (xTaskNotifyWait(0, 0xFFFFFFFF, &reply, pdMS_TO_TICKS(FEEDER_POLL_MS)) != pdTRUE) {
          if (state != STATE_PROCESSING || !SCAN_MODE) break;
        }

        // After D / A, Master owns the LCD.
        if (state == STATE_PROCESSING && SCAN_MODE) {
          if (reply == PI_REPLY_ACK) {
            pagesScanned++;
            sendLcd(SCREEN_PAGE_DONE);
          } else if (reply == PI_REPLY_NACK) {
            sendLcd(SCREEN_SCAN_ERR);
          }
        }

        // TODO: move the conveyor one full cycle.
      }
    }

    vTaskDelay(pdMS_TO_TICKS(FEEDER_POLL_MS));
  }
}

// TASK 3b: CONVEYOR TASK
// Scan mode only. Paper arriving at the entrance starts the belt and tells the
// Pi (CONV_START); from then on the Pi stops/adjusts it. Safety timeout if not.
bool paperAtEntrance() { return digitalRead(ENTRANCE_IR_PIN) == LOW; }

void setConveyor(uint8_t duty) { analogWrite(CONVEYOR_ENA_PIN, duty); }

void ConveyorTask(void *pvParameters) {
  bool running = false;
  bool paperWasPresent = false;
  uint8_t presentPolls = 0;
  uint32_t runStartMs = 0;
  uint8_t duty;

  for (;;) {
    if (!(state == STATE_PROCESSING && SCAN_MODE)) {
      // Outside a scan session: belt off, forget stale state and commands.
      if (running) { setConveyor(0); running = false; }
      paperWasPresent = false;  // Paper already waiting starts the belt next session
      presentPolls = 0;
      while (xQueueReceive(conveyorCmdQueue, &duty, 0) == pdTRUE) {}
      vTaskDelay(pdMS_TO_TICKS(FEEDER_POLL_MS));
      continue;
    }

    // Pi commands: stop, resume, change speed. Each run command restarts the timeout.
    while (xQueueReceive(conveyorCmdQueue, &duty, 0) == pdTRUE) {
      setConveyor(duty);
      running = duty > 0;
      runStartMs = millis();
    }

    // Debounced entrance sensor; start only on paper arriving, so a page the
    // Pi already stopped doesn't restart the belt.
    presentPolls = paperAtEntrance() ? min<uint8_t>(presentPolls + 1, ENTRANCE_DEBOUNCE_POLLS) : 0;
    bool paperPresent = presentPolls >= ENTRANCE_DEBOUNCE_POLLS;
    if (paperPresent && !paperWasPresent && !running) {
      setConveyor(CONVEYOR_DEFAULT_SPEED);
      running = true;
      runStartMs = millis();
      sendToPi("CONV_START");
    }
    paperWasPresent = paperPresent;

    if (running && millis() - runStartMs >= CONVEYOR_TIMEOUT_MS) {
      setConveyor(0);
      running = false;
      sendToPi("CONV_TIMEOUT");
    }

    vTaskDelay(pdMS_TO_TICKS(FEEDER_POLL_MS));
  }
}

// Door servo (not used yet). Two positions only; write() jumps straight to
// the angle, so the door snaps across at full servo speed.
// Servo doorServo;
// const uint8_t DOOR_CLOSED_ANGLE = 0;
// const uint8_t DOOR_OPEN_ANGLE = 90;   // Tune to door travel
// const uint32_t DOOR_TRAVEL_MS = 500;  // Full-swing time; tune to servo
//
// void doorOpen()  { doorServo.write(DOOR_OPEN_ANGLE); }
// void doorClose() { doorServo.write(DOOR_CLOSED_ANGLE); }
//
// // Open, hold, close.
// void doorCycle(uint32_t holdMs) {
//   doorOpen();
//   vTaskDelay(pdMS_TO_TICKS(DOOR_TRAVEL_MS + holdMs));
//   doorClose();
//   vTaskDelay(pdMS_TO_TICKS(DOOR_TRAVEL_MS));
// }

// Water fill (not used yet).
// const bool WATER_FULL_LEVEL = LOW;             // Sensor reading when full; flip if inverted
// const uint32_t WATER_FILL_TIMEOUT_MS = 30000;  // Safety cutoff (dry reservoir / stuck sensor)
//
// bool waterIsFull() { return digitalRead(WATER_LEVEL_PIN) == WATER_FULL_LEVEL; }
//
// // Pump until full or timeout. Returns true if filled.
// bool fillWater() {
//   uint32_t start = millis();
//   digitalWrite(WATER_PUMP_PIN, HIGH);
//   while (!waterIsFull() && millis() - start < WATER_FILL_TIMEOUT_MS) {
//     vTaskDelay(pdMS_TO_TICKS(50));
//   }
//   digitalWrite(WATER_PUMP_PIN, LOW);
//   return waterIsFull();
// }

// TASK 4: PULPER TASK
void PulperTask(void *pvParameters) {
  for (;;) {
    // Wait for Master to trigger pulping
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // TODO: pulper hardware
    // if (!fillWater()) {
    //   // Didn't fill: skip pulping / show error
    // }
    // digitalWrite(PULPER_RELAY_PIN, HIGH);   // Pulper on

    vTaskDelay(pdMS_TO_TICKS(3000)); // Simulated pulping

    // digitalWrite(PULPER_RELAY_PIN, LOW);    // Pulper off
    // doorCycle(1000);                         // Open, hold 1s, close
  }
}

void sendLcd(LcdScreen screen) {
  xQueueSend(lcdQueue, &screen, portMAX_DELAY);
}

// Read and handle one NMEA line from a Pi port, if any.
void handlePiPort(Stream &port) {
  if (!port.available()) return;

  String command = port.readStringUntil('\n');
  NmeaNode *nmea = parseNmea0183(command.c_str());
  if (nmea == NULL) return;

  // Page scanned OK
  if (nmea->token == "SCAN_ACK") {
    xTaskNotify(feederTaskHandle, PI_REPLY_ACK, eSetValueWithOverwrite);
  }

  // Page failed: release Feeder so the user can retry
  else if (nmea->token == "NACK") {
    xTaskNotify(feederTaskHandle, PI_REPLY_NACK, eSetValueWithOverwrite);
  }

  // Conveyor control: $CONV_STOP,<id> / $CONV_RUN,<id> / $CONV_SPEED,<id>,<0-255>
  else if (nmea->token == "CONV_STOP" || nmea->token == "CONV_RUN" || nmea->token == "CONV_SPEED") {
    uint8_t duty = 0;
    if (nmea->token == "CONV_RUN") {
      duty = CONVEYOR_DEFAULT_SPEED;
    } else if (nmea->token == "CONV_SPEED") {
      NmeaNode *arg = nmea->next ? nmea->next->next : NULL;  // Skip message ID
      if (arg == NULL) { freeNmeaList(nmea); return; }
      duty = (uint8_t)constrain(atoi(arg->token.c_str()), 0, 255);
    }
    xQueueSend(conveyorCmdQueue, &duty, 0);
  }

  freeNmeaList(nmea);
}

// TASK 5: MASTER ORCHESTRATION TASK
void MasterTask(void *pvParameters) {
  state = STATE_STANDBY;
  sendLcd(SCREEN_STANDBY);
  char key;

  for (;;) {
    if (xQueueReceive(keyQueue, &key, pdMS_TO_TICKS(10))) {

      // Global cancel, except during PIN entry where A is a PIN character
      if (key == 'A' && state != STATE_INPUT_PIN && state != STATE_CONFIRM_PIN) {
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
          // Any key counts (0-9, A-D, *, #); all 10 must be entered
          if (pinIndex < 10) {
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
          if (pinIndex < 10) {
            confirmPIN[pinIndex++] = key;
            sendLcd(SCREEN_CONFIRM_PIN);

            if (pinIndex == 10) {
              confirmPIN[10] = '\0';
              if (strcmp(PININPUT, confirmPIN) == 0) {
                sendLcd(SCREEN_PIN_SUCCESS);
                // Hex-encode: '*' is the NMEA checksum delimiter and would break the frame
                char pinHex[21];
                for (int i = 0; i < 10; i++) {
                  snprintf(&pinHex[i * 2], 3, "%02X", (uint8_t)PININPUT[i]);
                }
                sendToPi("SCANMODE", pinHex);

                // Wipe the PIN from RAM right after sending
                memset(PININPUT, 0, sizeof(PININPUT));
                memset(confirmPIN, 0, sizeof(confirmPIN));
                memset(pinHex, 0, sizeof(pinHex));

                vTaskDelay(pdMS_TO_TICKS(1500));
                pagesScanned = 0;
                state = STATE_PROCESSING;
                sendLcd(SCREEN_SCAN_READY);  // Feeder now waits for C

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
          // Temporary manual trigger: scan current page
          if (key == SCAN_TRIGGER_KEY && SCAN_MODE) {
            xSemaphoreGive(scanTriggerSem);
          }

          // D: done feeding this batch
          if (key == 'D') {
            if (SCAN_MODE) {
              sendToPi("SCAN_DONE");  // Pi closes the file/session
            }
            SCAN_MODE = 0;

            sendLcd(SCREEN_DONE);
            vTaskDelay(pdMS_TO_TICKS(2000));

            state = STATE_STANDBY;
            sendLcd(SCREEN_STANDBY);
          }
          break;
      }
    }

    // Pi replies on Serial1; USB / UART bridge also accepted for PC testing.
    handlePiPort(Serial1);
    handlePiPort(Serial);
    handlePiPort(Serial0);
  }
}

void setup() {
  Serial.begin(115200);
  Serial0.begin(115200);  // "UART" USB port (monitoring)
  Serial1.begin(115200, SERIAL_8N1, PI_UART_RX_PIN, PI_UART_TX_PIN);  // Pi link

  pinMode(SCAN_FLASH_PIN, OUTPUT);
  digitalWrite(SCAN_FLASH_PIN, LOW);

  pinMode(ENTRANCE_IR_PIN, INPUT);  // TCRT5000 module drives its own output
  pinMode(CONVEYOR_ENA_PIN, OUTPUT);
  setConveyor(0);

  // Pulper (not used yet); all outputs start off, door closed
  // pinMode(PULPER_RELAY_PIN, OUTPUT);
  // digitalWrite(PULPER_RELAY_PIN, LOW);
  // ESP32PWM::allocateTimer(1);               // Avoid LEDC timer 0 (IR TX carrier)
  // doorServo.setPeriodHertz(50);
  // doorServo.attach(PULPER_SERVO_PIN, 500, 2500);  // 500-2500us pulse range
  // doorServo.write(DOOR_CLOSED_ANGLE);
  // pinMode(WATER_LEVEL_PIN, INPUT_PULLUP);
  // pinMode(WATER_PUMP_PIN, OUTPUT);
  // digitalWrite(WATER_PUMP_PIN, LOW);

  irInit();

  Wire.begin(1, 2);
  lcd.init();
  lcd.backlight();

  keyQueue = xQueueCreate(10, sizeof(char));
  lcdQueue = xQueueCreate(10, sizeof(LcdScreen));
  scanTriggerSem = xSemaphoreCreateBinary();
  piTxMutex = xSemaphoreCreateMutex();
  conveyorCmdQueue = xQueueCreate(8, sizeof(uint8_t));

  xTaskCreatePinnedToCore(LcdTask, "LcdTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(KeypadTask, "KeypadTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(FeederTask, "FeederTask", 4096, NULL, 1, &feederTaskHandle, 1);
  xTaskCreatePinnedToCore(ConveyorTask, "ConveyorTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(PulperTask, "PulperTask", 4096, NULL, 1, &pulperTaskHandle, 1);
  xTaskCreatePinnedToCore(MasterTask, "MasterTask", 4096, NULL, 2, &masterTaskHandle, 1);
}

void loop() { vTaskDelete(NULL); }
