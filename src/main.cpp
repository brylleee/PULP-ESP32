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
constexpr uint8_t LCD_SDA_PIN = 1;
constexpr uint8_t LCD_SCL_PIN = 2;
constexpr uint16_t LCD_I2C_TIMEOUT_MS = 20;
constexpr uint32_t LCD_RETRY_MS = 250;  // redraw this often while the LCD doesn't answer
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

// Fixed limits
const uint32_t CONVEYOR_MANUAL_MAX_MS = 30000;  // CONV_RUN cap
const uint8_t ENTRANCE_DEBOUNCE_POLLS = 3;
const uint32_t SHREDDER_MAX_MS = 30000;  // SHRED cap

// Adjustable settings, kept in flash (Preferences namespace "settings"), so
// they survive reboots and reflashes. From the Pi:
//   SET,<id>,<name>,<value>  saves one; the board answers SETTING (or SETTING_ERR)
//   UNSET,<id>,<name>        back to the default; answers SETTING
//   GET,<id>[,<name>]        answers SETTING,<id>,<name>,<value>,<default>,<min>,<max>
//                            for that one, or for all of them
// A new value applies from the next time it is used (the belt speed at once).
enum SettingId : uint8_t {
  SET_PULP_WATER_MS, SET_PULP_CLOSE_MS, SET_PULP_BLEND_MS, SET_PULP_REST_MS, SET_PULP_BLENDS,
  SET_PULP_SETTLE_MS,
  SET_SHRED_MS, SET_SHRED_REST_MS,
  SET_BELT_SPEED, SET_BELT_STOP_MS, SET_BELT_EJECT_MS, SET_BELT_SHRED_MS, SET_FEED_TIMEOUT_MS,
  SET_DOOR_OPEN_DEG, SET_DOOR_CLOSED_DEG,
  SET_WATER_HIGH,
  SETTING_COUNT
};
struct SettingDef {
  const char *name;  // also the flash key: 15 characters at most
  uint32_t def, min, max;
};
const SettingDef SETTINGS[SETTING_COUNT] = {
  /* PULP_WATER_MS   */ {"pulp_water_ms", 12000, 0, 60000},     // pump run
  /* PULP_CLOSE_MS   */ {"pulp_close_ms", 1000, 0, 10000},      // door closing, before the blender
  /* PULP_BLEND_MS   */ {"pulp_blend_ms", 5000, 500, 60000},    // each blender run
  /* PULP_REST_MS    */ {"pulp_rest_ms", 3000, 0, 60000},       // between blender runs
  /* PULP_BLENDS     */ {"pulp_blends", 5, 1, 50},              // blender runs per cycle
  /* PULP_SETTLE_MS  */ {"pulp_settle_ms", 5000, 0, 60000},     // after the last run, before the door opens
  /* SHRED_MS        */ {"shred_ms", 10000, 1000, 30000},       // shredder run: both modes, D, SHRED's default
  /* SHRED_REST_MS   */ {"shred_rest_ms", 3000, 0, 30000},      // shredder rest after a run
  /* BELT_SPEED      */ {"belt_speed", 70, 1, 255},             // PWM duty for feeds and ejects
  /* BELT_STOP_MS    */ {"belt_stop_ms", 175, 0, 5000},         // scan feed: run on after the page clears the IR
  /* BELT_EJECT_MS   */ {"belt_eject_ms", 5000, 500, 30000},    // scan: carry the page to the shredder
  /* BELT_SHRED_MS   */ {"belt_shred_ms", 10000, 500, 30000},   // shred only: carry each page to the shredder
  /* FEED_TIMEOUT_MS */ {"feed_timeout_ms", 10000, 2000, 30000}, // a feed gives up (Page not found)
  /* DOOR_OPEN_DEG   */ {"door_open_deg", 90, 0, 180},          // pulper door servo, open (at rest)
  /* DOOR_CLOSED_DEG */ {"door_closed_deg", 0, 0, 180},         // ... closed (while blending)
  /* WATER_HIGH      */ {"water_high", 0, 0, 1},                // 1: the water sensor's HIGH means water
};
volatile uint32_t settingValue[SETTING_COUNT];
static inline uint32_t cfg(SettingId id) { return settingValue[id]; }

// Water sensor (on/off, at the bottom of the container the pump draws from).
// A new reading counts once it held for WATER_STABLE_SAMPLES in a row, so
// ripples from the pump don't flip it.
const uint32_t WATER_SAMPLE_MS = 200;
const uint8_t WATER_STABLE_SAMPLES = 5;  // 1 s

// STATUS_REPORT goes to the Pi this often, and right away on a change.
const uint32_t TELEMETRY_PERIOD_MS = 1000;
const uint32_t TELEMETRY_MIN_GAP_MS = 100;

enum SystemState {
  STATE_STANDBY,
  STATE_MENU,
  STATE_INPUT_PIN,
  STATE_CONFIRM_PIN,
  STATE_PROCESSING,
  STATE_FINISHING,     // after D
  STATE_OFF            // POWER_OFF: everything off, # turns it back on
};
const char *const STATE_NAMES[] = {"standby", "menu", "pin", "confirm_pin", "processing", "finishing", "off"};

// Off: a key other than # lights the LCD this long
const uint32_t OFF_LIGHT_MS = 5000;

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
  SCREEN_OFF,
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
  /* OFF         */ {2, "PULP is off", 0, "Press # to start"},
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
volatile bool lcdLight = true;     // backlight; dark while off
volatile bool pulpAbort = false;   // POWER_OFF stops a pulping cycle
volatile bool lcdResetRequest = false;  // the Pi's LCD_RESET: full re-init on the next redraw
volatile uint32_t lcdRecoveries = 0;    // times the LCD stopped answering and was brought back

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

static void conveyorSend(ConveyorEventType type, uint32_t ms = 0, uint8_t duty = 0) {
  ConveyorEvent evt = {type, duty, ms};
  xQueueSend(conveyorEvtQueue, &evt, 0);
}

Preferences settingsPrefs;  // namespace "settings"; only MasterTask writes it

int settingFind(const std::string &name) {
  for (int i = 0; i < SETTING_COUNT; i++) {
    if (name == SETTINGS[i].name) return i;
  }
  return -1;
}

// At boot. Values saved by older firmware (speed, stop_ms, water_high in
// "pulp") are taken over until the setting is saved anew.
void settingsLoad() {
  prefs.begin("pulp", true);
  uint32_t legacySpeed = prefs.getUChar("speed", SETTINGS[SET_BELT_SPEED].def);
  uint32_t legacyStop = prefs.getUShort("stop_ms", SETTINGS[SET_BELT_STOP_MS].def);
  uint32_t legacyWater = prefs.getBool("water_high", false) ? 1 : 0;
  prefs.end();

  settingsPrefs.begin("settings", true);
  for (int i = 0; i < SETTING_COUNT; i++) {
    const SettingDef &d = SETTINGS[i];
    uint32_t fallback = i == SET_BELT_SPEED ? legacySpeed : i == SET_BELT_STOP_MS ? legacyStop
                      : i == SET_WATER_HIGH ? legacyWater : d.def;
    uint32_t v = settingsPrefs.isKey(d.name) ? settingsPrefs.getUInt(d.name, fallback) : fallback;
    settingValue[i] = (v < d.min || v > d.max) ? d.def : v;
  }
  settingsPrefs.end();
}

void sendSetting(int i) {
  const SettingDef &d = SETTINGS[i];
  sendToPi("SETTING", std::string(d.name), std::to_string(settingValue[i]), std::to_string(d.def),
           std::to_string(d.min), std::to_string(d.max));
}

bool pulperBusy();

// Applies a new value where it matters at once. old: the value before.
static void settingApply(int i, uint32_t old) {
  if (i == SET_BELT_SPEED && (beltMode == BELT_FEED || beltMode == BELT_EJECT)) {
    hwConveyorDuty(cfg(SET_BELT_SPEED));
  }
  // A door resting open moves to the new open angle
  if (i == SET_DOOR_OPEN_DEG && !pulperBusy() && hwDoorAngleNow() == old) {
    hwDoorAngle(cfg(SET_DOOR_OPEN_DEG));
  }
}

// false: out of range. save=false (UNSET) removes the saved value.
bool settingSet(int i, uint32_t value, bool save = true) {
  const SettingDef &d = SETTINGS[i];
  if (value < d.min || value > d.max) return false;
  uint32_t old = settingValue[i];
  settingValue[i] = value;
  settingsPrefs.begin("settings", false);
  if (save) settingsPrefs.putUInt(d.name, value); else settingsPrefs.remove(d.name);
  settingsPrefs.end();
  if (i == SET_BELT_SPEED || i == SET_BELT_STOP_MS || i == SET_WATER_HIGH) {
    // also drop what older firmware saved, so UNSET really means the default
    prefs.begin("pulp", false);
    prefs.remove(i == SET_BELT_SPEED ? "speed" : i == SET_BELT_STOP_MS ? "stop_ms" : "water_high");
    prefs.end();
  }
  settingApply(i, old);
  return true;
}

// ms = 0 means belt_eject_ms, duty = 0 the saved speed.
// Overrides the current belt mode.
void conveyorRun(uint32_t ms = 0, uint8_t duty = 0) {
  if (duty == 0) duty = cfg(SET_BELT_SPEED);
  if (ms == 0) ms = cfg(SET_BELT_EJECT_MS);
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

// ms = 0 means shred_ms. Waits out a rest if one is running.
void shredderRun(uint32_t ms = 0) {
  if (ms == 0) ms = cfg(SET_SHRED_MS);
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

void doorOpen()  { hwDoorAngle(cfg(SET_DOOR_OPEN_DEG)); }
void doorClose() { hwDoorAngle(cfg(SET_DOOR_CLOSED_DEG)); }
void pumpOn()    { hwPump(true); }
void pumpOff()   { hwPump(false); }
void blenderOn() { hwBlender(true); }
void blenderOff(){ hwBlender(false); }

bool pulperBusy() { return pulpPhase != PULP_IDLE; }

// Ignored while a cycle is running.
void pulperRun() {
  if (!pulperBusy()) xTaskNotifyGive(pulperTaskHandle);
}

// Water in the container, kept up to date by TelemetryTask
volatile bool waterLow = true;  // no water at the sensor; true until the first reading
bool waterIsLow() { return waterLow; }

// Scan whatever is under the camera
void scanTrigger() { xSemaphoreGive(scanTriggerSem); }

// Power. Off: belt, shredder, pulper, pump and flash stopped, LCD dark, Pi
// commands ignored and the keypad too, except # (back on). Saved in flash,
// so a reboot stays off. Telemetry keeps going.
void savePowerOff(bool off) {
  prefs.begin("pulp", false);
  prefs.putBool("off", off);
  prefs.end();
}

bool savedPowerOff() {
  prefs.begin("pulp", true);
  bool off = prefs.getBool("off", false);
  prefs.end();
  return off;
}

void machinePowerOff() {
  if (state == STATE_OFF) return;
  bool scanning = state == STATE_PROCESSING && SCAN_MODE;
  state = STATE_OFF;
  SCAN_MODE = 0;
  pinIndex = 0;
  memset(PININPUT, 0, sizeof(PININPUT));
  memset(confirmPIN, 0, sizeof(confirmPIN));
  conveyorStop();
  shredderStop();
  if (pulperBusy()) pulpAbort = true;
  hwAllOff();  // now; the tasks catch up within 50 ms
  if (scanning) sendToPi("SCAN_DONE");  // the Pi keeps the pages scanned so far
  lcdLight = false;
  sendLcd(SCREEN_OFF);
  savePowerOff(true);
}

// fromKeypad: # was pressed. Tells the Pi, and pulses PI_WAKE in case the Pi
// is halted.
void machinePowerOn(bool fromKeypad) {
  if (state != STATE_OFF) return;
  state = STATE_STANDBY;
  lcdLight = true;
  sendLcd(SCREEN_STANDBY);
  savePowerOff(false);
  if (fromKeypad) {
    sendToPi("POWER_ON");
    hwPiWakePulse();
  }
}

void machineRestart() {
  hwAllOff();
  Serial.flush();
  delay(100);
  ESP.restart();
}

// STATUS_REPORT,<id>,<state>,<mode>,<flash>,<belt>,<duty>,<shredder>,<paper>,<pages>,<pulper>,
//               <speed>,<water>
// <water>: 1 while the sensor detects water, 0 when the container is empty
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
           std::to_string(cfg(SET_BELT_SPEED)),
           std::to_string(waterLow ? 0 : 1));
}

// ---------------------------------------------------------------------
// Pi commands
// ---------------------------------------------------------------------

// Argument after the message ID, as text ("" when missing)
std::string nmeaArgText(NmeaNode *nmea, int index) {
  NmeaNode *node = nmea->next ? nmea->next->next : nullptr;
  for (int i = 0; node && i < index; i++) node = node->next;
  return node ? node->token : std::string();
}

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

  // Off: only the power commands and STATUS
  if (state == STATE_OFF && cmd != "POWER_ON" && cmd != "POWER_OFF" && cmd != "RESTART" && cmd != "STATUS") {
    freeNmeaList(nmea);
    return;
  }

  if (cmd == "POWER_OFF") {
    machinePowerOff();
  } else if (cmd == "POWER_ON") {
    machinePowerOn(false);
  } else if (cmd == "RESTART") {
    machineRestart();
  } else if (cmd == "SCAN_ACK") {
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
    if (nmeaArgUint(nmea, 0, a)) settingSet(SET_BELT_SPEED, a);  // same as SET belt_speed
  } else if (cmd == "WATER_ACTIVE") {  // 0: LOW means water, 1: HIGH means water
    if (nmeaArgUint(nmea, 0, a)) settingSet(SET_WATER_HIGH, a);  // same as SET water_high
  } else if (cmd == "CONV_STOP_MS") {
    if (nmeaArgUint(nmea, 0, a)) settingSet(SET_BELT_STOP_MS, a);  // same as SET belt_stop_ms
  } else if (cmd == "CONV_STOP") {
    conveyorStop();
  } else if (cmd == "SHRED") {
    if (!nmeaArgUint(nmea, 0, a)) a = 0;
    shredderRun(a);
  } else if (cmd == "SHRED_STOP") {
    shredderStop();
  } else if (cmd == "PULP") {
    pulperRun();
  } else if (cmd == "DOOR") {  // 1 = open, 0 = close; ignored mid-cycle
    if (nmeaArgUint(nmea, 0, a) && a <= 1 && !pulperBusy()) {
      if (a) doorOpen(); else doorClose();
    }
  } else if (cmd == "STATUS") {
    sendStatus();
  } else if (cmd == "SET" || cmd == "UNSET" || cmd == "GET") {
    std::string name = nmeaArgText(nmea, 0);
    int i = settingFind(name);
    if (cmd == "GET" && name.empty()) {
      for (int k = 0; k < SETTING_COUNT; k++) sendSetting(k);
    } else if (i < 0) {
      sendToPi("SETTING_ERR", name.empty() ? std::string("?") : name, std::string("unknown setting"));
    } else if (cmd == "GET") {
      sendSetting(i);
    } else if (cmd == "UNSET") {
      settingSet(i, SETTINGS[i].def, false);
      sendSetting(i);
    } else if (nmeaArgUint(nmea, 1, a) && settingSet(i, a)) {
      sendSetting(i);
    } else {
      sendToPi("SETTING_ERR", name, "must be " + std::to_string(SETTINGS[i].min) + "-" + std::to_string(SETTINGS[i].max));
    }
  } else if (cmd == "LCD_RESET") {  // frees the I2C bus and re-inits the LCD
    lcdResetRequest = true;
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
    uint8_t light = lcdLight ? LCD_BACKLIGHT : LCD_NOBACKLIGHT;
    Wire.write(n | light | En);
    Wire.write(n | light);
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

// Relay and motor switching can also leave the LCD's I2C bus stuck (a device
// holding SDA low mid-byte): every write then fails and the LCD keeps showing
// the last screen. Clock SCL until SDA is let go, send a STOP, restart Wire.
static void i2cRecover() {
  Wire.end();
  pinMode(LCD_SDA_PIN, INPUT_PULLUP);
  pinMode(LCD_SCL_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(LCD_SCL_PIN, HIGH);
  for (int i = 0; i < 9 && digitalRead(LCD_SDA_PIN) == LOW; i++) {
    digitalWrite(LCD_SCL_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(LCD_SCL_PIN, HIGH);
    delayMicroseconds(5);
  }
  pinMode(LCD_SDA_PIN, OUTPUT_OPEN_DRAIN);  // STOP: SDA rises while SCL is high
  digitalWrite(LCD_SDA_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(LCD_SDA_PIN, HIGH);
  delayMicroseconds(5);
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  Wire.setTimeOut(LCD_I2C_TIMEOUT_MS);
}

static bool lcdAnswers() {
  Wire.beginTransmission(LCD_ADDR);
  return Wire.endTransmission() == 0;
}

// Fill with spaces to the full width, so no clear() is needed
static void lcdPad(char *line) {
  size_t n = strlen(line);
  while (n < LCD_COLS) line[n++] = ' ';
  line[LCD_COLS] = '\0';
}

// LCD TASK: draws the screens the other tasks queue. Redraws the current one
// every LCD_REFRESH_MS too, so a glitched display fixes itself. When the LCD
// stops answering on I2C it frees the bus, re-inits it and retries often.
void LcdTask(void *pvParameters) {
  LcdScreen screen = SCREEN_STANDBY;
  LcdScreen next;
  char top[LCD_COLS + 1], bottom[LCD_COLS + 1];
  bool lcdOk = true;

  for (;;) {
    uint32_t wait = lcdOk ? LCD_REFRESH_MS : LCD_RETRY_MS;
    if (xQueueReceive(lcdQueue, &next, pdMS_TO_TICKS(wait))) screen = next;

    if (lcdResetRequest || !lcdAnswers()) {
      bool requested = lcdResetRequest;
      lcdResetRequest = false;
      i2cRecover();
      if (!lcdAnswers()) {  // still nothing: try again soon
        lcdOk = false;
        continue;
      }
      lcd.init();  // the controller may have lost its setup too
      if (!requested) lcdRecoveries++;
    }
    lcdOk = true;
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
    if (lcdLight) lcd.backlight(); else lcd.noBacklight();
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
  uint32_t onMs = cfg(SET_SHRED_MS);

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
    if (shredPhase == SHRED_REST && now - phaseStartMs >= cfg(SET_SHRED_REST_MS)) {
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
// Scan: paper in -> FEED + CONV_START,ir -> page clears the IR -> stop after
// cfg(SET_BELT_STOP_MS) -> SCAN -> ACK -> EJECT. A CONV_PAUSE from the Pi
// stops a feed early too.
// Shred only: paper in -> EJECT + shredder.
void ConveyorTask(void *pvParameters) {
  bool pageInFrame = false;   // held under the camera
  bool feedPending = false;   // next page waiting for the belt
  bool paperWasPresent = false;
  uint8_t presentPolls = 0;
  uint8_t clearPolls = 0;
  uint32_t clearSinceMs = 0;  // when the entrance IR last went clear
  uint32_t runStartMs = 0;
  uint32_t runMs = 0;
  ConveyorEvent evt;

  auto setMode = [&](BeltMode m, uint32_t ms = 0, uint8_t duty = 0) {
    if (duty == 0) duty = cfg(SET_BELT_SPEED);
    beltMode = m;
    runStartMs = millis();
    runMs = ms;
    hwConveyorDuty(m == BELT_OFF ? 0 : duty);
  };
  auto startFeed = [&]() {
    setMode(BELT_FEED, cfg(SET_FEED_TIMEOUT_MS));
    sendToPi("CONV_START", std::string("ir"));  // "ir": the board stops the belt itself
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
            setMode(BELT_EJECT, cfg(SET_BELT_EJECT_MS));
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
          setMode(BELT_EJECT, cfg(SET_BELT_SHRED_MS));
          shredderRun();
        } else if (beltMode == BELT_OFF && !pageInFrame) {
          startFeed();
        } else if (beltMode != BELT_FEED) {
          feedPending = true;
        }
      }
      paperWasPresent = paperPresent;

      // Back edge past the IR: stop the feed a set time later. Timed from the
      // first clear reading, confirmed by ENTRANCE_DEBOUNCE_POLLS of them.
      bool clear = !paperAtEntrance();
      if (clear && clearPolls == 0) clearSinceMs = millis();
      clearPolls = clear ? min<uint8_t>(clearPolls + 1, ENTRANCE_DEBOUNCE_POLLS) : 0;
      if (scanMode && beltMode == BELT_FEED && clearPolls >= ENTRANCE_DEBOUNCE_POLLS &&
          millis() - clearSinceMs >= cfg(SET_BELT_STOP_MS)) {
        setMode(BELT_OFF);
        pageInFrame = true;
        scanTrigger();
      }
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

// PULPER TASK: water, door closes, blender bursts, door opens, then PULP_DONE,<id>,ok
// The door stays open between cycles; it is closed only while the blender runs.
// (or water_low when the container was empty: the pump was skipped, or
// stopped as soon as the water ran out)
// POWER_OFF ends the cycle early: PULP_DONE,<id>,aborted.
void PulperTask(void *pvParameters) {
  // Waits in short steps; false once POWER_OFF asked to stop
  auto phase = [](PulpPhase p, uint32_t ms) {
    pulpPhase = p;
    // The pump stops the moment the container runs dry
    for (uint32_t waited = 0; waited < ms && !pulpAbort && !(p == PULP_WATER && waterIsLow()); waited += 50) {
      vTaskDelay(pdMS_TO_TICKS(min<uint32_t>(50, ms - waited)));
    }
    return !pulpAbort;
  };

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    pulpAbort = false;

    bool going = true;

    // A pump must not run dry: skip it when the container is low.
    bool pumped = !waterIsLow();
    if (pumped) {
      pumpOn();
      going = phase(PULP_WATER, cfg(SET_PULP_WATER_MS));
      pumpOff();
    }

    if (going) {
      doorClose();
      going = phase(PULP_DOOR, cfg(SET_PULP_CLOSE_MS));
    }

    const uint32_t runs = cfg(SET_PULP_BLENDS);
    for (uint32_t run = 1; going && run <= runs; run++) {
      blenderOn();
      going = phase(PULP_BLEND, cfg(SET_PULP_BLEND_MS));
      blenderOff();
      if (going && run < runs) going = phase(PULP_BLEND_REST, cfg(SET_PULP_REST_MS));
    }
    if (going) going = phase(PULP_DOOR, cfg(SET_PULP_SETTLE_MS));  // let it settle, closed
    doorOpen();  // also after an abort (at once): the blender is already off

    pulpPhase = PULP_IDLE;
    ulTaskNotifyTake(pdTRUE, 0);  // drop a PULP that came in mid-cycle
    bool ranDry = !pumped || waterIsLow();
    const char *result = !going ? "aborted" : ranDry ? "water_low" : "ok";
    pulpAbort = false;
    sendToPi("PULP_DONE", std::string(result));
  }
}

// Takes a new sensor state once it held WATER_STABLE_SAMPLES readings in a
// row. first: the boot reading, taken as is.
void readWaterSensor(bool first) {
  static uint8_t streak = 0;
  bool low = hwWaterPinHigh() != (cfg(SET_WATER_HIGH) != 0);  // no water
  if (first || low == waterLow) {
    waterLow = low;
    streak = 0;
  } else if (++streak >= WATER_STABLE_SAMPLES) {
    waterLow = low;
    streak = 0;
  }
}

// Everything STATUS_REPORT carries that should trigger a report at once
std::array<uint16_t, 10> statusSnapshot() {
  return {(uint16_t)state, (uint16_t)SCAN_MODE, flashIsOn(), beltMode, hwConveyorDutyNow(),
          shredPhase, paperAtEntrance(), pulpPhase, waterLow, pagesScanned};
}

// TELEMETRY TASK: water sensor, and STATUS_REPORT every TELEMETRY_PERIOD_MS
// and right away when the machine's state changes
void TelemetryTask(void *pvParameters) {
  uint32_t lastSent = 0;
  uint32_t lastSample = millis();
  readWaterSensor(true);
  auto lastSnapshot = statusSnapshot();

  for (;;) {
    uint32_t now = millis();
    if (now - lastSample >= WATER_SAMPLE_MS) {
      lastSample = now;
      readWaterSensor(false);
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
  if (state == STATE_OFF) return;  // POWER_OFF came in meanwhile

  sendLcd(SCREEN_DONE);
  vTaskDelay(pdMS_TO_TICKS(2000));
  state = STATE_STANDBY;
  sendLcd(SCREEN_STANDBY);
}

// MASTER TASK: the keypad state machine, and the Pi's commands
void MasterTask(void *pvParameters) {
  if (savedPowerOff()) {
    state = STATE_OFF;
    lcdLight = false;
    sendLcd(SCREEN_OFF);
  } else {
    state = STATE_STANDBY;
    sendLcd(SCREEN_STANDBY);
  }
  char key;
  uint32_t offLightAt = 0;

  for (;;) {
    if (xQueueReceive(keyQueue, &key, pdMS_TO_TICKS(10))) {
      bool enteringPin = state == STATE_INPUT_PIN || state == STATE_CONFIRM_PIN;

      if (state == STATE_OFF) {
        if (key == '#') {
          machinePowerOn(true);
        } else {  // show how to turn it on
          offLightAt = millis();
          lcdLight = true;
          sendLcd(SCREEN_OFF);
        }
      } else if (key == 'A' && !enteringPin) {
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

    if (state == STATE_OFF && lcdLight && millis() - offLightAt >= OFF_LIGHT_MS) {
      lcdLight = false;
      sendLcd(SCREEN_OFF);
    }

    servePi();
  }
}

void setup() {
  Serial.begin(115200);
  Serial0.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, PI_UART_RX_PIN, PI_UART_TX_PIN);

  hardwareInit();
  settingsLoad();
  hwDoorAngle(cfg(SET_DOOR_OPEN_DEG));  // the saved resting angle
  irInit();

  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  Wire.setTimeOut(LCD_I2C_TIMEOUT_MS);
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

