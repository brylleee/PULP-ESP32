#include "Keypad.h"
#include "Wire.h"
#include "LiquidCrystal_I2C.h"

#include "nmea-parser.h"

#include "ir_breakbeam.h"

#include "hardware.h"

#include <Preferences.h>
#include <algorithm>
#include <array>

constexpr uint8_t LCD_ADDR = 0x27;
constexpr uint8_t LCD_COLS = 16;
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, 2);

const byte rows = 4;
const byte columns = 4;

byte rowpins[rows] = {15, 16, 17, 18};
byte columnpins[columns] = {4, 5, 6, 7};

char keys[rows][columns] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

Keypad keypad = Keypad(makeKeymap(keys), rowpins, columnpins, rows, columns);

// Serial  - native USB
// Serial0 - "UART" USB bridge, monitoring only (bridge drives GPIO44)
// Serial1 - Pi link

const uint32_t FEEDER_POLL_MS = 20;
const char SCAN_TRIGGER_KEY = 'C';  // manual scan
const uint8_t PIN_LENGTH = 10;
const uint32_t LCD_REFRESH_MS = 2000;

// Conveyor
const uint8_t CONVEYOR_DEFAULT_SPEED = 70;      // PWM duty, 0-255; CONV_SPEED overrides
const uint32_t CONVEYOR_TIMEOUT_MS = 10000;     // feed gives up if no CONV_PAUSE
const uint32_t CONVEYOR_EJECT_MS = 5000;        // carry page to the shredder
const uint32_t CONVEYOR_MANUAL_MAX_MS = 30000;  // CONV_RUN cap
const uint8_t ENTRANCE_DEBOUNCE_POLLS = 3;

// Shredder: run, then rest
const uint32_t SHREDDER_ON_MS = 5000;
const uint32_t SHREDDER_REST_MS = 3000;
const uint32_t SHREDDER_MAX_MS = 30000;  // SHRED cap

// Pulper cycle
const uint32_t PULP_DOOR_OPEN_MS = 1000;
const uint32_t PULP_WATER_MS = 5000;
const uint32_t PULP_BLEND_ON_MS = 5000;
const uint32_t PULP_BLEND_REST_MS = 3000;
const uint8_t PULP_BLEND_RUNS = 10;

// Water container the pump draws from. Calibrate EMPTY/FULL from the raw
// value in STATUS_REPORT with the sensor dry and at the full mark.
const uint16_t WATER_RAW_EMPTY = 150;
const uint16_t WATER_RAW_FULL = 1800;
const uint8_t WATER_LOW_PERCENT = 20;   // below: low, the pulper skips the pump
const uint8_t WATER_OK_PERCENT = 25;    // above: ok again (no flicker in between)
const uint32_t WATER_SAMPLE_MS = 200;

// STATUS_REPORT goes to the Pi this often, and right away on a change.
const uint32_t TELEMETRY_PERIOD_MS = 1000;
const uint32_t TELEMETRY_MIN_GAP_MS = 100;

enum SystemState {
  STATE_STANDBY,
  STATE_MENU,
  STATE_INPUT_PIN,
  STATE_CONFIRM_PIN,
  STATE_PROCESSING,
  STATE_FINISHING      // after D
};
const char *const STATE_NAMES[] = {"standby", "menu", "pin", "confirm_pin", "processing", "finishing"};

enum BeltMode : uint8_t {
  BELT_OFF,
  BELT_FEED,    // to the camera
  BELT_EJECT,   // to the shredder
  BELT_MANUAL   // CONV_RUN
};
const char *const BELT_MODE_NAMES[] = {"off", "feed", "eject", "manual"};

enum ConveyorEventType : uint8_t { CONV_EVT_PAUSE, CONV_EVT_EJECT, CONV_EVT_RUN, CONV_EVT_STOP };
struct ConveyorEvent {
  ConveyorEventType type;
  uint8_t duty;
  uint32_t ms;
};

enum ShredPhase : uint8_t { SHRED_IDLE, SHRED_ON, SHRED_REST };
const char *const SHRED_PHASE_NAMES[] = {"idle", "on", "rest"};

enum PulpPhase : uint8_t { PULP_IDLE, PULP_DOOR, PULP_WATER, PULP_BLEND, PULP_BLEND_REST };
const char *const PULP_PHASE_NAMES[] = {"idle", "door", "water", "blend", "blend_rest"};

// SCAN reply, passed to Feeder as a notification value
enum PiReply : uint32_t { PI_REPLY_NONE, PI_REPLY_ACK, PI_REPLY_NACK };

enum LcdScreen {
  SCREEN_STANDBY,
  SCREEN_MENU,
  SCREEN_INPUT_PIN,
  SCREEN_CONFIRM_PIN,
  SCREEN_PIN_SUCCESS,
  SCREEN_PIN_ERR,
  SCREEN_SCAN_READY,
  SCREEN_FEEDING,
  SCREEN_NOT_FOUND,    // feed timed out
  SCREEN_SCANNING,
  SCREEN_PAGE_DONE,
  SCREEN_SCAN_ERR,
  SCREEN_FEED_READY,   // shred only
  SCREEN_SHREDDING,
  SCREEN_PULPING,
  SCREEN_DONE,
  SCREEN_LINK,         // B in standby: Pi RX counters
  SCREEN_COUNT
};

// Fixed text of each screen; LcdTask adds the numbers and PIN stars.
struct LcdText {
  uint8_t topCol;
  const char *top;
  uint8_t bottomCol;
  const char *bottom;
};
const LcdText LCD_TEXT[SCREEN_COUNT] = {
  /* STANDBY     */ {0, "[=] PULP Status:", 4, "Standby"},
  /* MENU        */ {0, "[1] Scan & Shred", 0, "[2] Shred only"},
  /* INPUT_PIN   */ {0, "Input Your PIN:", 0, "PIN:"},
  /* CONFIRM_PIN */ {0, "Confirm PIN:", 0, "PIN:"},
  /* PIN_SUCCESS */ {1, "PIN Confirmed!", 0, ""},
  /* PIN_ERR     */ {0, "PINs Don't Match", 0, ""},
  /* SCAN_READY  */ {0, "Insert paper", 0, "D:Finish"},
  /* FEEDING     */ {0, "Feeding page ", 0, "D:Finish"},
  /* NOT_FOUND   */ {0, "Page not found", 0, "C:Scan D:Finish"},
  /* SCANNING    */ {0, "Scanning page ", 0, "Please wait..."},
  /* PAGE_DONE   */ {0, "Page ", 0, "D:Finish"},
  /* SCAN_ERR    */ {0, "Failed. Pages:", 0, "C:Retry D:Finish"},
  /* FEED_READY  */ {0, "Insert paper", 0, "D:Finish"},
  /* SHREDDING   */ {0, "[=] PULP Status:", 4, "Shredding"},
  /* PULPING     */ {0, "[=] PULP Status:", 4, "Pulping"},
  /* DONE        */ {0, "[=] PULP Status:", 6, "Done"},
  /* LINK        */ {0, "Pi RX B:", 0, "ok:"},
};

// RTOS handles
QueueHandle_t keyQueue;
QueueHandle_t lcdQueue;
QueueHandle_t conveyorEvtQueue;
SemaphoreHandle_t scanTriggerSem;  // given on CONV_PAUSE or C, taken by Feeder
SemaphoreHandle_t piTxMutex;       // keeps frames from interleaving
TaskHandle_t feederTaskHandle = NULL;
TaskHandle_t pulperTaskHandle = NULL;

// Shared state
volatile SystemState state;
bool SCAN_MODE = 0;
char PININPUT[PIN_LENGTH + 1];
char confirmPIN[PIN_LENGTH + 1];
volatile byte pinIndex = 0;  // written by Master, read by LCD
volatile uint16_t pagesScanned = 0;

// Pi link counters (Serial1 only), shown with B
volatile uint32_t piRxBytes = 0;
volatile uint32_t piRxOk = 0;
volatile uint32_t piRxBad = 0;

// Send a frame to the Pi, mirrored to the USB ports.
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

void sendLcd(LcdScreen screen) {
  xQueueSend(lcdQueue, &screen, portMAX_DELAY);
}

// ---------------------------------------------------------------------
// Actions: called by both the tasks and Pi commands. See README.md.
// ---------------------------------------------------------------------

// Flash
void flashOn()  { hwFlash(true); }
void flashOff() { hwFlash(false); }
bool flashIsOn() { return hwFlashIsOn(); }

// Conveyor
volatile BeltMode beltMode = BELT_OFF;  // owned by ConveyorTask

// Belt speed for feeds and ejects, saved in flash
Preferences prefs;
volatile uint8_t conveyorSpeed = CONVEYOR_DEFAULT_SPEED;

static void conveyorSend(ConveyorEventType type, uint32_t ms = 0, uint8_t duty = 0) {
  ConveyorEvent evt = {type, duty, ms};
  xQueueSend(conveyorEvtQueue, &evt, 0);
}

void conveyorLoadSpeed() {
  prefs.begin("pulp", true);
  conveyorSpeed = prefs.getUChar("speed", CONVEYOR_DEFAULT_SPEED);
  prefs.end();
}

// Also applies to a feed or eject already running
void conveyorSetSpeed(uint8_t duty) {
  if (duty == 0) return;
  conveyorSpeed = duty;
  if (beltMode == BELT_FEED || beltMode == BELT_EJECT) hwConveyorDuty(duty);
  prefs.begin("pulp", false);
  prefs.putUChar("speed", duty);
  prefs.end();
}

// ms = 0 means CONVEYOR_EJECT_MS, duty = 0 the saved speed.
// Overrides the current belt mode.
void conveyorRun(uint32_t ms = 0, uint8_t duty = 0) {
  if (duty == 0) duty = conveyorSpeed;
  if (ms == 0) ms = CONVEYOR_EJECT_MS;
  if (ms > CONVEYOR_MANUAL_MAX_MS) ms = CONVEYOR_MANUAL_MAX_MS;
  conveyorSend(CONV_EVT_RUN, ms, duty);
}

void conveyorStop() {
  hwConveyorDuty(0);  // cut now, don't wait for the task
  conveyorSend(CONV_EVT_STOP);
}

// Page centred. Cut the motor here, the task poll is too slow.
void conveyorPauseForScan() {
  if (beltMode == BELT_FEED) hwConveyorDuty(0);
  conveyorSend(CONV_EVT_PAUSE);
}

void conveyorEject() { conveyorSend(CONV_EVT_EJECT); }

bool paperAtEntrance() { return hwPaperAtEntrance(); }

// Shredder (state guarded by shredMux)
portMUX_TYPE shredMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool shredRequest = false;
volatile uint32_t shredRequestMs = 0;
volatile bool shredStopRequest = false;
volatile ShredPhase shredPhase = SHRED_IDLE;

// ms = 0 means SHREDDER_ON_MS. Waits out a rest if one is running.
void shredderRun(uint32_t ms = 0) {
  if (ms == 0) ms = SHREDDER_ON_MS;
  if (ms > SHREDDER_MAX_MS) ms = SHREDDER_MAX_MS;
  taskENTER_CRITICAL(&shredMux);
  shredRequest = true;
  shredRequestMs = ms;
  taskEXIT_CRITICAL(&shredMux);
}

// Ends the run early; the rest still applies.
void shredderStop() {
  taskENTER_CRITICAL(&shredMux);
  shredRequest = false;
  shredStopRequest = true;
  taskEXIT_CRITICAL(&shredMux);
}

// Pending or running (rest doesn't count)
bool shredderBusy() {
  taskENTER_CRITICAL(&shredMux);
  bool busy = shredRequest || shredPhase == SHRED_ON;
  taskEXIT_CRITICAL(&shredMux);
  return busy;
}

// Pulper
volatile PulpPhase pulpPhase = PULP_IDLE;  // owned by PulperTask

void doorOpen()  { hwDoorAngle(DOOR_OPEN_ANGLE); }
void doorClose() { hwDoorAngle(DOOR_CLOSED_ANGLE); }
void pumpOn()    { hwPump(true); }
void pumpOff()   { hwPump(false); }
void blenderOn() { hwBlender(true); }
void blenderOff(){ hwBlender(false); }

bool pulperBusy() { return pulpPhase != PULP_IDLE; }

// Ignored while a cycle is running.
void pulperRun() {
  if (!pulperBusy()) xTaskNotifyGive(pulperTaskHandle);
}

// Water level, kept up to date by TelemetryTask
volatile uint16_t waterRaw = 0;
volatile uint8_t waterPercent = 0;
volatile bool waterLow = true;  // until the first reading
bool waterIsLow() { return waterLow; }

// Scan whatever is under the camera
void scanTrigger() { xSemaphoreGive(scanTriggerSem); }

// STATUS_REPORT,<id>,<state>,<mode>,<flash>,<belt>,<duty>,<shredder>,<paper>,<pages>,<pulper>,
//               <speed>,<water %>,<water raw>,<water low>
void sendStatus() {
  const char *mode = (state == STATE_PROCESSING) ? (SCAN_MODE ? "scan" : "shred") : "none";
  sendToPi("STATUS_REPORT",
           std::string(STATE_NAMES[state]),
           std::string(mode),
           std::to_string(flashIsOn() ? 1 : 0),
           std::string(BELT_MODE_NAMES[beltMode]),
           std::to_string(hwConveyorDutyNow()),
           std::string(SHRED_PHASE_NAMES[shredPhase]),
           std::to_string(paperAtEntrance() ? 1 : 0),
           std::to_string(pagesScanned),
           std::string(PULP_PHASE_NAMES[pulpPhase]),
           std::to_string(conveyorSpeed),
           std::to_string(waterPercent),
           std::to_string(waterRaw),
           std::to_string(waterLow ? 1 : 0));
}

// ---------------------------------------------------------------------
// Pi commands
// ---------------------------------------------------------------------

// Argument after the message ID, as an unsigned int
bool nmeaArgUint(NmeaNode *nmea, int index, uint32_t &out) {
  NmeaNode *node = nmea->next ? nmea->next->next : nullptr;
  for (int i = 0; node && i < index; i++) node = node->next;
  if (!node || node->token.empty()) return false;
  char *end = nullptr;
  unsigned long value = strtoul(node->token.c_str(), &end, 10);
  if (*end != '\0' || node->token[0] == '-') return false;
  out = value;
  return true;
}

void handlePiPort(Stream &port) {
  if (!port.available()) return;

  String command = port.readStringUntil('\n');
  command.trim();
  if (command.isEmpty()) return;
  NmeaNode *nmea = parseNmea0183(command.c_str());
  // Log what came in, to debug the link from the USB monitor
  // (the Pi ACKs every frame; those would drown the log)
  if (!nmea || nmea->token != "ACK") Serial.printf("[rx %s] %s\n", nmea ? "ok" : "bad", command.c_str());
  if (&port == &Serial1) {
    piRxBytes += command.length();
    if (nmea) piRxOk++; else piRxBad++;
  }
  if (nmea == NULL) return;

  const std::string &cmd = nmea->token;
  uint32_t a = 0, b = 0;

  if (cmd == "SCAN_ACK") {
    xTaskNotify(feederTaskHandle, PI_REPLY_ACK, eSetValueWithOverwrite);
  } else if (cmd == "NACK") {
    xTaskNotify(feederTaskHandle, PI_REPLY_NACK, eSetValueWithOverwrite);
  } else if (cmd == "CONV_PAUSE") {
    conveyorPauseForScan();
  } else if (cmd == "FLASH") {
    if (nmeaArgUint(nmea, 0, a) && a <= 1) {
      if (a) flashOn(); else flashOff();
    }
  } else if (cmd == "CONV_RUN") {
    if (!nmeaArgUint(nmea, 0, a)) a = 0;
    if (!nmeaArgUint(nmea, 1, b) || b > 255) b = 0;
    conveyorRun(a, (uint8_t)b);
  } else if (cmd == "CONV_SPEED") {
    if (nmeaArgUint(nmea, 0, a) && a >= 1 && a <= 255) conveyorSetSpeed((uint8_t)a);
  } else if (cmd == "CONV_STOP") {
    conveyorStop();
  } else if (cmd == "SHRED") {
    if (!nmeaArgUint(nmea, 0, a)) a = 0;
    shredderRun(a);
  } else if (cmd == "SHRED_STOP") {
    shredderStop();
  } else if (cmd == "PULP") {
    pulperRun();
  } else if (cmd == "STATUS") {
    sendStatus();
  }

  freeNmeaList(nmea);
}

// The Pi on Serial1; the USB ports too, for testing from a PC
void servePi() {
  handlePiPort(Serial1);
  handlePiPort(Serial);
  handlePiPort(Serial0);
}

// ---------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------

// Redo the HD44780 4-bit handshake. Relay and motor switching can knock the
// LCD out of nibble sync (garbled text, display shifted left) and clear()
// can't fix that. Takes ~10 ms; lcd.init() takes over 1 s.
static void lcdResync() {
  auto nibble = [](uint8_t n) {
    Wire.beginTransmission(LCD_ADDR);
    Wire.write(n | LCD_BACKLIGHT | En);
    Wire.write(n | LCD_BACKLIGHT);
    Wire.endTransmission();
  };
  nibble(0x30); delayMicroseconds(4500);
  nibble(0x30); delayMicroseconds(4500);
  nibble(0x30); delayMicroseconds(150);
  nibble(0x20);
  lcd.command(LCD_FUNCTIONSET | LCD_4BITMODE | LCD_2LINE | LCD_5x8DOTS);
  lcd.command(LCD_DISPLAYCONTROL | LCD_DISPLAYON);
  lcd.command(LCD_ENTRYMODESET | LCD_ENTRYLEFT);
  lcd.home();  // also undoes a display shift
}

// Fill with spaces to the full width, so no clear() is needed
static void lcdPad(char *line) {
  size_t n = strlen(line);
  while (n < LCD_COLS) line[n++] = ' ';
  line[LCD_COLS] = '\0';
}

// LCD TASK: draws the screens the other tasks queue. Redraws the current one
// every LCD_REFRESH_MS too, so a glitched display fixes itself.
void LcdTask(void *pvParameters) {
  LcdScreen screen = SCREEN_STANDBY;
  LcdScreen next;
  char top[LCD_COLS + 1], bottom[LCD_COLS + 1];

  for (;;) {
    if (xQueueReceive(lcdQueue, &next, pdMS_TO_TICKS(LCD_REFRESH_MS))) screen = next;
    const LcdText &text = LCD_TEXT[screen];

    int n = snprintf(top, sizeof(top), "%*s%s", text.topCol, "", text.top);
    char *end = top + n;
    size_t room = sizeof(top) - n;
    switch (screen) {
      case SCREEN_FEEDING:
      case SCREEN_SCANNING:  snprintf(end, room, "%u", pagesScanned + 1); break;
      case SCREEN_PAGE_DONE: snprintf(end, room, "%u scanned!", pagesScanned); break;
      case SCREEN_SCAN_ERR:  snprintf(end, room, "%u", pagesScanned); break;
      case SCREEN_LINK:      snprintf(end, room, "%lu", (unsigned long)piRxBytes); break;
      default: break;
    }

    n = snprintf(bottom, sizeof(bottom), "%*s%s", text.bottomCol, "", text.bottom);
    if (screen == SCREEN_INPUT_PIN || screen == SCREEN_CONFIRM_PIN) {
      for (int i = 0; i < pinIndex && n < LCD_COLS; i++) bottom[n++] = '*';
      bottom[n] = '\0';
    } else if (screen == SCREEN_LINK) {
      snprintf(bottom + n, sizeof(bottom) - n, "%lu bad:%lu",
               (unsigned long)piRxOk, (unsigned long)piRxBad);
    }

    lcdPad(top);
    lcdPad(bottom);
    lcdResync();
    lcd.setCursor(0, 0);
    lcd.print(top);
    lcd.setCursor(0, 1);
    lcd.print(bottom);
  }
}

// KEYPAD TASK: polls the keypad every 50 ms
void KeypadTask(void *pvParameters) {
  for (;;) {
    char key = keypad.getKey();
    if (key) {
      Serial.println(key);
      xQueueSend(keyQueue, &key, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// FEEDER TASK: scan flash, and asks the Pi to scan each page
void FeederTask(void *pvParameters) {
  bool sessionFlash = false;

  for (;;) {
    // Only switch on session start/end so a Pi FLASH sticks
    bool wantFlash = state == STATE_PROCESSING && SCAN_MODE;
    if (wantFlash != sessionFlash) {
      sessionFlash = wantFlash;
      if (wantFlash) flashOn(); else flashOff();
    }

    if (state == STATE_PROCESSING && SCAN_MODE) {
      // Short timeout so we notice the session ending
      if (xSemaphoreTake(scanTriggerSem, pdMS_TO_TICKS(FEEDER_POLL_MS)) != pdTRUE) {
        continue;
      }

      // Clear stale replies
      xTaskNotifyWait(0, 0xFFFFFFFF, NULL, 0);

      sendToPi("SCAN");
      sendLcd(SCREEN_SCANNING);

      // No timeout; only bail if the session ends
      uint32_t reply = PI_REPLY_NONE;
      while (xTaskNotifyWait(0, 0xFFFFFFFF, &reply, pdMS_TO_TICKS(FEEDER_POLL_MS)) != pdTRUE) {
        if (state != STATE_PROCESSING || !SCAN_MODE) break;
      }

      // After D/A the LCD belongs to Master
      if (state == STATE_PROCESSING && SCAN_MODE) {
        if (reply == PI_REPLY_ACK) {
          pagesScanned++;
          conveyorEject();
          shredderRun();
          sendLcd(SCREEN_PAGE_DONE);
        } else if (reply == PI_REPLY_NACK) {
          sendLcd(SCREEN_SCAN_ERR);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(FEEDER_POLL_MS));
  }
}

// SHREDDER TASK: owns the relay; runs, then rests
void ShredderTask(void *pvParameters) {
  uint32_t phaseStartMs = 0;
  uint32_t onMs = SHREDDER_ON_MS;

  for (;;) {
    uint32_t now = millis();

    taskENTER_CRITICAL(&shredMux);
    if (shredStopRequest) {
      shredStopRequest = false;
      if (shredPhase == SHRED_ON) {
        shredPhase = SHRED_REST;
        phaseStartMs = now;
      }
    }
    if (shredPhase == SHRED_REST && now - phaseStartMs >= SHREDDER_REST_MS) {
      shredPhase = SHRED_IDLE;
    }
    if (shredRequest && shredPhase != SHRED_REST) {
      shredRequest = false;
      int32_t laterBy = (int32_t)((now + shredRequestMs) - (phaseStartMs + onMs));
      if (shredPhase != SHRED_ON || laterBy > 0) {
        // Start, or extend if the new end is later
        shredPhase = SHRED_ON;
        phaseStartMs = now;
        onMs = shredRequestMs;
      }
    } else if (shredPhase == SHRED_ON && now - phaseStartMs >= onMs) {
      shredPhase = SHRED_REST;
      phaseStartMs = now;
    }
    bool on = shredPhase == SHRED_ON;
    taskEXIT_CRITICAL(&shredMux);

    if (on != hwShredderRelayIsOn()) hwShredderRelay(on);
    vTaskDelay(pdMS_TO_TICKS(FEEDER_POLL_MS));
  }
}

// CONVEYOR TASK: owns the belt
// Scan: paper in -> FEED + CONV_START -> CONV_PAUSE -> SCAN -> ACK -> EJECT.
// Shred only: paper in -> EJECT + shredder.
void ConveyorTask(void *pvParameters) {
  bool pageInFrame = false;   // held under the camera
  bool feedPending = false;   // next page waiting for the belt
  bool paperWasPresent = false;
  uint8_t presentPolls = 0;
  uint32_t runStartMs = 0;
  uint32_t runMs = 0;
  ConveyorEvent evt;

  auto setMode = [&](BeltMode m, uint32_t ms = 0, uint8_t duty = 0) {
    if (duty == 0) duty = conveyorSpeed;
    beltMode = m;
    runStartMs = millis();
    runMs = ms;
    hwConveyorDuty(m == BELT_OFF ? 0 : duty);
  };
  auto startFeed = [&]() {
    setMode(BELT_FEED, CONVEYOR_TIMEOUT_MS);
    sendToPi("CONV_START");
    sendLcd(SCREEN_FEEDING);
  };

  for (;;) {
    bool active = state == STATE_PROCESSING;
    bool scanMode = active && SCAN_MODE;

    while (xQueueReceive(conveyorEvtQueue, &evt, 0) == pdTRUE) {
      switch (evt.type) {
        case CONV_EVT_PAUSE:
          if (scanMode && beltMode == BELT_FEED) {
            setMode(BELT_OFF);
            pageInFrame = true;
            scanTrigger();
          }
          break;
        case CONV_EVT_EJECT:
          if (scanMode) {
            setMode(BELT_EJECT, CONVEYOR_EJECT_MS);
            pageInFrame = false;
          }
          break;
        case CONV_EVT_RUN:
          setMode(BELT_MANUAL, evt.ms, evt.duty);
          pageInFrame = false;
          break;
        case CONV_EVT_STOP:
          setMode(BELT_OFF);
          feedPending = false;
          break;
      }
    }

    if (!active) {
      // Outside a session. Pi runs and the eject after D keep going.
      bool keep = beltMode == BELT_MANUAL || (beltMode == BELT_EJECT && state == STATE_FINISHING);
      if (beltMode != BELT_OFF && !keep) setMode(BELT_OFF);
      pageInFrame = false;
      feedPending = false;
      paperWasPresent = false;  // paper already waiting gets fed next session
      presentPolls = 0;
    } else {
      // Debounced, rising edge only
      presentPolls = paperAtEntrance() ? min<uint8_t>(presentPolls + 1, ENTRANCE_DEBOUNCE_POLLS) : 0;
      bool paperPresent = presentPolls >= ENTRANCE_DEBOUNCE_POLLS;
      if (paperPresent && !paperWasPresent) {
        sendToPi("PAGE_IN");  // the Pi counts the sheets shredded
        if (!scanMode) {
          setMode(BELT_EJECT, CONVEYOR_EJECT_MS);
          shredderRun();
        } else if (beltMode == BELT_OFF && !pageInFrame) {
          startFeed();
        } else if (beltMode != BELT_FEED) {
          feedPending = true;
        }
      }
      paperWasPresent = paperPresent;
    }

    if (beltMode != BELT_OFF && millis() - runStartMs >= runMs) {
      if (beltMode == BELT_FEED) {
        setMode(BELT_OFF);
        sendToPi("CONV_TIMEOUT");
        sendLcd(SCREEN_NOT_FOUND);
      } else if (feedPending && scanMode) {
        feedPending = false;
        startFeed();
      } else {
        setMode(BELT_OFF);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(FEEDER_POLL_MS));
  }
}

// PULPER TASK: door, water, blender bursts, then PULP_DONE,<id>,ok
// (or water_low when the container was low and the pump was skipped)
void PulperTask(void *pvParameters) {
  auto phase = [](PulpPhase p, uint32_t ms) {
    pulpPhase = p;
    vTaskDelay(pdMS_TO_TICKS(ms));
  };

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    doorOpen();
    phase(PULP_DOOR, PULP_DOOR_OPEN_MS);
    doorClose();

    // A pump must not run dry: skip it when the container is low.
    bool pumped = !waterIsLow();
    if (pumped) {
      pumpOn();
      phase(PULP_WATER, PULP_WATER_MS);
      pumpOff();
    }

    for (uint8_t run = 1; run <= PULP_BLEND_RUNS; run++) {
      blenderOn();
      phase(PULP_BLEND, PULP_BLEND_ON_MS);
      blenderOff();
      if (run < PULP_BLEND_RUNS) phase(PULP_BLEND_REST, PULP_BLEND_REST_MS);
    }

    pulpPhase = PULP_IDLE;
    ulTaskNotifyTake(pdTRUE, 0);  // drop a PULP that came in mid-cycle
    sendToPi("PULP_DONE", std::string(pumped ? "ok" : "water_low"));
  }
}

// Median of 5 readings (ignores the odd spike), scaled to 0-100 %, with a
// gap between the low and ok thresholds so the state doesn't flicker.
void readWaterLevel(bool first) {
  uint16_t r[5];
  for (auto &v : r) v = hwWaterRaw();
  std::sort(r, r + 5);
  waterRaw = r[2];

  int span = WATER_RAW_FULL - WATER_RAW_EMPTY;
  int pct = span > 0 ? (int(r[2]) - WATER_RAW_EMPTY) * 100 / span : 0;
  waterPercent = constrain(pct, 0, 100);

  if (waterPercent < WATER_LOW_PERCENT) waterLow = true;
  else if (waterPercent > WATER_OK_PERCENT) waterLow = false;
  else if (first) waterLow = false;  // in the gap at boot: not low yet
}

// Everything STATUS_REPORT carries that should trigger a report at once
// (not the water percentage, which wobbles)
std::array<uint16_t, 10> statusSnapshot() {
  return {(uint16_t)state, (uint16_t)SCAN_MODE, flashIsOn(), beltMode, hwConveyorDutyNow(),
          shredPhase, paperAtEntrance(), pulpPhase, waterLow, pagesScanned};
}

// TELEMETRY TASK: water level, and STATUS_REPORT every TELEMETRY_PERIOD_MS
// and right away when the machine's state changes
void TelemetryTask(void *pvParameters) {
  uint32_t lastSent = 0;
  uint32_t lastSample = millis();
  readWaterLevel(true);
  auto lastSnapshot = statusSnapshot();

  for (;;) {
    uint32_t now = millis();
    if (now - lastSample >= WATER_SAMPLE_MS) {
      lastSample = now;
      readWaterLevel(false);
    }

    auto snapshot = statusSnapshot();
    bool changed = snapshot != lastSnapshot;
    if ((changed && now - lastSent >= TELEMETRY_MIN_GAP_MS) || now - lastSent >= TELEMETRY_PERIOD_MS) {
      lastSnapshot = snapshot;
      lastSent = now;
      sendStatus();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// PIN entered twice and matching: open the scan session on the Pi.
void startScanSession() {
  sendLcd(SCREEN_PIN_SUCCESS);
  // Hex, since '*' would end the NMEA sentence
  char pinHex[PIN_LENGTH * 2 + 1];
  for (int i = 0; i < PIN_LENGTH; i++) {
    snprintf(&pinHex[i * 2], 3, "%02X", (uint8_t)PININPUT[i]);
  }
  sendToPi("SCANMODE", pinHex);

  // Wipe the PIN
  memset(PININPUT, 0, sizeof(PININPUT));
  memset(confirmPIN, 0, sizeof(confirmPIN));
  memset(pinHex, 0, sizeof(pinHex));

  vTaskDelay(pdMS_TO_TICKS(1500));
  pagesScanned = 0;
  state = STATE_PROCESSING;
  sendLcd(SCREEN_SCAN_READY);
}

// One key of the PIN or its confirmation. Every key counts, all 10 required.
void pinKey(char key) {
  bool confirming = state == STATE_CONFIRM_PIN;
  char *pin = confirming ? confirmPIN : PININPUT;
  pin[pinIndex++] = key;
  sendLcd(confirming ? SCREEN_CONFIRM_PIN : SCREEN_INPUT_PIN);
  if (pinIndex < PIN_LENGTH) return;

  pin[PIN_LENGTH] = '\0';
  pinIndex = 0;
  if (!confirming) {
    vTaskDelay(pdMS_TO_TICKS(500));
    memset(confirmPIN, 0, sizeof(confirmPIN));
    state = STATE_CONFIRM_PIN;
    sendLcd(SCREEN_CONFIRM_PIN);
  } else if (strcmp(PININPUT, confirmPIN) == 0) {
    startScanSession();
  } else {
    sendLcd(SCREEN_PIN_ERR);
    vTaskDelay(pdMS_TO_TICKS(1500));
    state = STATE_STANDBY;
    sendLcd(SCREEN_STANDBY);
  }
}

// D: close the session, final shredder run, back to standby.
void finishSession() {
  if (SCAN_MODE) sendToPi("SCAN_DONE");
  SCAN_MODE = 0;
  state = STATE_FINISHING;

  // Final run to clear the chamber
  shredderRun();
  sendLcd(SCREEN_SHREDDING);
  while (shredderBusy()) {
    vTaskDelay(pdMS_TO_TICKS(50));
    servePi();
  }

  sendLcd(SCREEN_DONE);
  vTaskDelay(pdMS_TO_TICKS(2000));
  state = STATE_STANDBY;
  sendLcd(SCREEN_STANDBY);
}

// MASTER TASK: the keypad state machine, and the Pi's commands
void MasterTask(void *pvParameters) {
  state = STATE_STANDBY;
  sendLcd(SCREEN_STANDBY);
  char key;

  for (;;) {
    if (xQueueReceive(keyQueue, &key, pdMS_TO_TICKS(10))) {
      bool enteringPin = state == STATE_INPUT_PIN || state == STATE_CONFIRM_PIN;

      if (key == 'A' && !enteringPin) {
        // A cancels, except during PIN entry
        if (state == STATE_PROCESSING) shredderRun();  // clear the chamber
        conveyorStop();
        SCAN_MODE = 0;
        state = STATE_STANDBY;
        sendLcd(SCREEN_STANDBY);
      } else if (enteringPin) {
        pinKey(key);
      } else if (state == STATE_STANDBY) {
        if (key == '#') {
          state = STATE_MENU;
          sendLcd(SCREEN_MENU);
        } else if (key == 'B') {
          sendLcd(SCREEN_LINK);
          vTaskDelay(pdMS_TO_TICKS(3000));
          sendLcd(SCREEN_STANDBY);
        }
      } else if (state == STATE_MENU) {
        if (key == '1') {  // Scan & Shred
          pinIndex = 0;
          memset(PININPUT, 0, sizeof(PININPUT));
          SCAN_MODE = 1;
          state = STATE_INPUT_PIN;
          sendLcd(SCREEN_INPUT_PIN);
        } else if (key == '2') {  // Shred only
          SCAN_MODE = 0;
          state = STATE_PROCESSING;
          sendLcd(SCREEN_FEED_READY);
        }
      } else if (state == STATE_PROCESSING) {
        if (key == SCAN_TRIGGER_KEY && SCAN_MODE) scanTrigger();  // retries, missed pages
        if (key == 'D') finishSession();
      }
    }

    servePi();
  }
}

void setup() {
  Serial.begin(115200);
  Serial0.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, PI_UART_RX_PIN, PI_UART_TX_PIN);

  hardwareInit();
  conveyorLoadSpeed();
  irInit();

  Wire.begin(1, 2);
  lcd.init();
  lcd.backlight();

  keyQueue = xQueueCreate(10, sizeof(char));
  lcdQueue = xQueueCreate(10, sizeof(LcdScreen));
  scanTriggerSem = xSemaphoreCreateBinary();
  piTxMutex = xSemaphoreCreateMutex();
  conveyorEvtQueue = xQueueCreate(8, sizeof(ConveyorEvent));

  xTaskCreatePinnedToCore(LcdTask, "LcdTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(KeypadTask, "KeypadTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(FeederTask, "FeederTask", 4096, NULL, 1, &feederTaskHandle, 1);
  xTaskCreatePinnedToCore(ShredderTask, "ShredderTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(ConveyorTask, "ConveyorTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(PulperTask, "PulperTask", 4096, NULL, 1, &pulperTaskHandle, 1);
  xTaskCreatePinnedToCore(TelemetryTask, "TelemetryTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(MasterTask, "MasterTask", 4096, NULL, 2, NULL, 1);
}

void loop() { vTaskDelete(NULL); }

