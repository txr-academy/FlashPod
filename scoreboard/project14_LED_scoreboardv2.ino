#define PIXEL_COLOR_DEPTH_BITS 4 
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

/* ── NUS UUIDs ──────────────────────────────────────────── */
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define PANEL_RES_X 64     
#define PANEL_RES_Y 64     
#define PANEL_CHAIN 1      

MatrixPanel_I2S_DMA *dma_display = nullptr;

/* ── BLE & Game State ───────────────────────────────────── */
static BLEServer         *pServer         = nullptr;
static BLECharacteristic *pTxChar         = nullptr;
static bool               deviceConnected = false;
bool lastBtState = false;

int hits = 0;
int rounds = 0;

uint16_t base_Black, base_White, base_Gray;
uint16_t base_Yellow, base_Blue, base_Red, base_Green, base_Cyan;
uint16_t color_Text, color_Line, color_Hit, color_Miss;
uint16_t color_StatConn, color_StatDisc, color_Reward;

int colorMode = 0; 

String formatDigits(int num) {
  return (num < 10 ? "0" : "") + String(num);
}

void applyColorPalette() {
  Serial.println("\n--------------------------------");
  if (colorMode == 0) {
    Serial.println("Palette Mode: NORMAL VISION");
    color_Text     = base_White;
    color_Line     = base_Yellow;
    color_Hit      = base_Green;
    color_Miss     = base_Red;
    color_StatConn = base_Green;
    color_StatDisc = base_Red;
    color_Reward   = base_Yellow;
  } 
  else if (colorMode == 1) {
    Serial.println("Palette Mode: RED-GREEN BLIND");
    color_Text     = base_White;
    color_Line     = base_Yellow;
    color_Hit      = base_White;
    color_Miss     = base_Blue;
    color_StatConn = base_Blue;
    color_StatDisc = base_Blue;
    color_Reward   = base_Yellow;
  }
  else if (colorMode == 2) {
    Serial.println("Palette Mode: BLUE-YELLOW BLIND");
    color_Text     = base_White;
    color_Line     = base_Red;
    color_Hit      = base_Cyan;
    color_Miss     = base_Red;
    color_StatConn = base_Cyan;
    color_StatDisc = base_Red;
    color_Reward   = base_Cyan;
  }
  else if (colorMode == 3) {
    Serial.println("Palette Mode: MONOCHROME");
    color_Text     = base_White;
    color_Line     = base_White;
    color_Hit      = base_White;
    color_Miss     = base_Gray;
    color_StatConn = base_White;
    color_StatDisc = base_Gray;
    color_Reward   = base_White;
  }
  Serial.println("--------------------------------\n");
}

void updateDisplay() {
  dma_display->fillScreen(base_Black);
  dma_display->drawLine(0, 63, 63, 0, color_Line);
  
  dma_display->setTextSize(3);
  dma_display->setTextColor(color_Text);
  dma_display->setCursor(4, 4);
  dma_display->print(formatDigits(hits));

  dma_display->setTextColor(color_Text);
  dma_display->setCursor(28, 38);
  dma_display->print(formatDigits(rounds));

  // Visual status indicator
  if (deviceConnected) {
    dma_display->fillRect(60, 60, 4, 4, color_StatConn); 
  } else {
    dma_display->drawRect(60, 60, 4, 4, color_StatDisc); 
  }
}

void processCommand(char cmd) {
  if (cmd == 'q' || cmd == 'Q' || cmd == '0') {
    colorMode = (colorMode + 1) % 4; 
    applyColorPalette();
    updateDisplay();
    return; 
  }

  bool updated = false;

  if (cmd == 'R' || cmd == 'r') {
    hits = 0;
    rounds = 0;
    Serial.println("\n--- Game Reset ---");
    updated = true;
  }
  else if (rounds < 99) {
    if (cmd == '1' || cmd == 'A' || cmd == 'a') {
      hits++;
      rounds++;
      Serial.println("Player HIT!");

      for (int r = 0; r <= 46; r += 2) {
        dma_display->drawCircle(32, 32, r, color_Hit);
        if (r > 1) dma_display->drawCircle(32, 32, r - 1, color_Hit); 
        if (r > 4) dma_display->drawCircle(32, 32, r - 4, base_Black); 
        delay(15); 
      }
      delay(50);
      updated = true;
    } 
    else if (cmd == '2' || cmd == 'D' || cmd == 'd') {
      rounds++;
      Serial.println("Player MISSED!");
      
      for (int pulse = 0; pulse < 2; pulse++) {
        for (int i = 0; i < 4; i++) {
          dma_display->drawRect(i, i, 64 - (2 * i), 64 - (2 * i), color_Miss);
          delay(40);
        }
        for (int i = 3; i >= 0; i--) {
          dma_display->drawRect(i, i, 64 - (2 * i), 64 - (2 * i), base_Black);
          delay(40);
        }
      }
      updated = true;
    }
  } 

  if (updated) {
    updateDisplay();
    Serial.print("Current Score -> Hits: ");
    Serial.print(hits);
    Serial.print(" / Rounds Played: ");
    Serial.println(rounds);
  }
}

/* ── BLE Callbacks ──────────────────────────────────────── */
class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pSvr) override {
        deviceConnected = true;
        Serial.println("BLE Client connected");
    }

    void onDisconnect(BLEServer *pSvr) override {
        deviceConnected = false;
        pSvr->startAdvertising();
        Serial.println("BLE Client disconnected — advertising restarted");
    }
};

class RxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        // Direct assignment works cleanly across ESP32 core versions
        String input = pChar->getValue();
        input.trim(); 

        if (input.length() == 0) return;

        if (input.length() == 1) {
            processCommand(input[0]); 
        } 
        else if (input.indexOf('/') > 0) {
            int slashIndex = input.indexOf('/');
            hits = input.substring(0, slashIndex).toInt();
            rounds = input.substring(slashIndex + 1).toInt();
            updateDisplay(); 
            Serial.println("Direct Score Update Applied via BLE!");
        }
    }
};

/* ── Setup & Loop ───────────────────────────────────────── */
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n Starting ");
  
  // 1. Start BLE First (Crucial to prevent DMA memory corruption)
  Serial.println("[1/3] Starting BLE Server");
  BLEDevice::init("GAME_Scoreboard");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(NUS_SERVICE_UUID);

  pTxChar = pService->createCharacteristic(
      NUS_TX_UUID,
      BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxChar->addDescriptor(new BLE2902());

  BLECharacteristic *pRxChar = pService->createCharacteristic(
      NUS_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_NR
  );
  pRxChar->setCallbacks(new RxCallbacks());

  pService->start();
  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(NUS_SERVICE_UUID);
  pAdv->setScanResponse(true);
  BLEDevice::startAdvertising();

  // 2. Initialize Matrix Second
  Serial.println("[2/3] Initializing LED Matrix");
  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);
  mxconfig.gpio.e = 18; 
  mxconfig.double_buff = false;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(200);
  Serial.println("Matrix successfully initialized");

  base_Black  = dma_display->color565(0, 0, 0);
  base_White  = dma_display->color565(255, 255, 255);
  base_Gray   = dma_display->color565(80, 80, 80);
  base_Yellow = dma_display->color565(255, 220, 0);
  base_Blue   = dma_display->color565(0, 150, 255); 
  base_Red    = dma_display->color565(255, 0, 0); 
  base_Green  = dma_display->color565(0, 255, 0); 
  base_Cyan   = dma_display->color565(0, 255, 255); 

  applyColorPalette();
  updateDisplay();
  
  Serial.println("[3/3] Panel is READY.");
}

void loop() {
  if (deviceConnected != lastBtState) {
    lastBtState = deviceConnected;
    updateDisplay();
    
    if (deviceConnected) {
      Serial.println("\n[BLUETOOTH] Client successfully CONNECTED");
    } else {
      Serial.println("\n[BLUETOOTH] Client DISCONNECTED");
    }
  }

  // Backup control via hardware Serial monitor
  while (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    
    if (input.length() == 1) {
      processCommand(input[0]);
    }
    else if (input.indexOf('/') > 0) {
      int slashIndex = input.indexOf('/');
      hits = input.substring(0, slashIndex).toInt();
      rounds = input.substring(slashIndex + 1).toInt();
      updateDisplay();
      Serial.println("Direct Score Update Applied via Serial!");
    }
  }
  
  delay(20);
}