#include "Keypad.h"
#include "Wire.h"
#include "LiquidCrystal_I2C.h"

#include "nmea-parser.h"

LiquidCrystal_I2C lcd(0x27, 16, 4); 

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

QueueHandle_t keyQueue;
QueueHandle_t lcdQueue;
TaskHandle_t masterTaskHandle = NULL;
TaskHandle_t feederTaskHandle = NULL;
TaskHandle_t pulperTaskHandle = NULL;

// Global States; Read by LCD, Written by Master) 
char PININPUT[11];
char confirmPIN[11];
volatile byte pinIndex = 0; 

bool SCAN_MODE = 0;

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
  SCREEN_SCANNING,
  SCREEN_SHREDDING,
  SCREEN_PULPING,
  SCREEN_DONE
};

SystemState state;

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
        case SCREEN_SCANNING:
          lcd.setCursor(2, 1); lcd.print("[=] PULP Status:");
          lcd.setCursor(4, 2); lcd.print("Scanning");
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
      Serial.println(key);
      xQueueSend(keyQueue, &key, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(50)); 
  }
}

// TASK 3: INPUT FEEDER TASK
void FeederTask(void *pvParameters) {
  for(;;) {
    // If we're actively taking in papers
    if(state == STATE_PROCESSING) {
      // Check if outer input feeder IR is blocked (paper in)
      // Then roll the roller pins

      // Stop if inner input feeder IR is blocked but not outer.
      // It means: paper is about to go fully inside

      // Take a picture
      if(SCAN_MODE) {
        // Turn on LED here...

        sendNmea0183(Serial, "SCAN");

        // Waits for ACK...
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Then move the conveyor belt one full cycle.
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
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

// TASK 5: MASTER ORCHESTRATION TASK
void MasterTask(void *pvParameters) {
  state = STATE_STANDBY;
  sendLcd(SCREEN_STANDBY);
  char key;

  for (;;) {
    // Process Keypad Queue
    if (xQueueReceive(keyQueue, &key, pdMS_TO_TICKS(10))) {
      
      if (key == 'A') { // Global cancel button
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
                sendNmea0183(Serial, "SCANMODE", PININPUT);
                
                // Zeroize PIN in RAM immediately after transmission
                memset(PININPUT, 0, sizeof(PININPUT));
                memset(confirmPIN, 0, sizeof(confirmPIN));
                
                vTaskDelay(pdMS_TO_TICKS(1500));
                state = STATE_PROCESSING;
                
                // Wake up Feeder Task
                sendLcd(SCREEN_SCANNING);
                xTaskNotifyGive(feederTaskHandle);
                
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
          // User presses 'D' to indicate they are done feeding the batch of papers
          if (key == 'D') { 
            if (SCAN_MODE) {
              // Tell the Raspberry Pi to close the file/session
              sendNmea0183(Serial, "SCAN_DONE");
            }
            
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

    // Serial comms from Raspberry Pi
    if (Serial.available()) {
      String command = Serial.readStringUntil('\n');
      NmeaNode *nmea = parseNmea0183(command.c_str());

      if(nmea == NULL) continue;

      std::string cmd = nmea->token.c_str();
      
      // Tells ESP32 an individual page scan is successful
      if(cmd == "SCAN_ACK") {
        xTaskNotifyGive(feederTaskHandle);
      }

      delete nmea;
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  Wire.begin(1, 2); 
  lcd.init();
  lcd.backlight();
  
  keyQueue = xQueueCreate(10, sizeof(char));
  lcdQueue = xQueueCreate(10, sizeof(LcdScreen));

  xTaskCreatePinnedToCore(LcdTask, "LcdTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(KeypadTask, "KeypadTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(FeederTask, "FeederTask", 4096, NULL, 1, &feederTaskHandle, 1);
  xTaskCreatePinnedToCore(PulperTask, "PulperTask", 4096, NULL, 1, &pulperTaskHandle, 1);
  xTaskCreatePinnedToCore(MasterTask, "MasterTask", 4096, NULL, 2, &masterTaskHandle, 1);
}

void loop() { vTaskDelete(NULL); }