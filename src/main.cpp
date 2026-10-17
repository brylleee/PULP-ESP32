#include "Keypad.h"
#include "Wire.h"
#include "LiquidCrystal_I2C.h"

#include "nmea-parser.h"

LiquidCrystal_I2C lcd(0x27, 16, 4); 

const byte rows = 4;
const byte columns = 4;

byte rowpins[rows] = {4, 5, 6, 7};
byte columnpins[columns] = {15, 16, 17, 18};
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
      xQueueSend(keyQueue, &key, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(50)); 
  }
}

// TASK 3: INPUT FEEDER TASK
void FeederTask(void *pvParameters) {
  for (;;) {
    // Sleep and wait for Master Task to trigger feeding
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    
    // TODO: Add IR sensor checks and DC motor relay logic here
    
    vTaskDelay(pdMS_TO_TICKS(2000)); // Simulate feeding duration
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
  SystemState state = STATE_STANDBY;
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
                // Transmit PIN to RPi ("Hermes") over UART
                Serial.print("PIN_SET:");
                Serial.println(PININPUT);
                
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
          break;
      }
    }

    // Serial comms with Raspberry Pi
    if (Serial.available()) {
      String command = Serial.readStringUntil('\n');
      command.trim();
      
      // Parse comms here
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