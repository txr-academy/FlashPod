#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Adafruit_NeoPixel.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

/* ── Pod identity — CHANGE THIS per ESP32 ───────────────── */
#define POD_NUMBER   3
#define POD_NAME     "Blazepod3"

/* ── Pin config ─────────────────────────────────────────── */
#define LED_PIN      16
#define IR_PIN       13
#define NUM_PIXELS   16
#define IR_POLL_MS   50

/* ── NUS UUIDs ──────────────────────────────────────────── */
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

/* ── NeoPixel ───────────────────────────────────────────── */
Adafruit_NeoPixel strip(NUM_PIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

/* ── BLE objects ────────────────────────────────────────── */
static BLEServer         *pServer         = nullptr;
static BLECharacteristic *pTxChar         = nullptr;
static bool               deviceConnected = false;

/* ── Shared state ───────────────────────────────────────── */
static portMUX_TYPE   stateMux    = portMUX_INITIALIZER_UNLOCKED;
static bool           ledActive   = false;
static unsigned long  ledOnTimeMs = 0;
static unsigned long  timeoutMs   = 10000;  /* default 10s, updated by nRF */

/* ── IR task handle ─────────────────────────────────────── */
static TaskHandle_t irTaskHandle = nullptr;

/* ══════════════════════════════════════════════════════════
 * LED helpers
 * ══════════════════════════════════════════════════════════ */
void ledSetAll(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < NUM_PIXELS; i++) {
        strip.setPixelColor(i, strip.Color(r, g, b));
    }
    strip.show();
}

void ledOff()
{
    strip.clear();
    strip.show();
}

/* ══════════════════════════════════════════════════════════
 * BLE send helper
 * ══════════════════════════════════════════════════════════ */
void sendToNrf(const char *msg)
{
    if (!deviceConnected || pTxChar == nullptr) return;
    pTxChar->setValue((uint8_t *)msg, strlen(msg));
    pTxChar->notify();
}

/* ══════════════════════════════════════════════════════════
 * BLE Server callbacks
 * ══════════════════════════════════════════════════════════ */
class ServerCallbacks : public BLEServerCallbacks
{
    void onConnect(BLEServer *pSvr) override
    {
        deviceConnected = true;
        Serial.println("nRF connected");
    }

    void onDisconnect(BLEServer *pSvr) override
    {
        deviceConnected = false;

        portENTER_CRITICAL(&stateMux);
        ledActive = false;
        portEXIT_CRITICAL(&stateMux);

        ledOff();
        delay(500);
        pSvr->startAdvertising();
        Serial.println("nRF disconnected — advertising restarted");
    }
};

/* ══════════════════════════════════════════════════════════
 * RX callbacks — receives commands from nRF central
 *
 * Commands:
 *   ON:RRGGBB      → turn LED on with hex colour
 *   OFF            → turn LED off
 *   TIMEOUT:xxxxx  → set timeout in milliseconds
 * ══════════════════════════════════════════════════════════ */
class RxCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pChar) override
    {
        std::string value = pChar->getValue();
        if (value.empty()) return;

        /* Strip trailing newline/carriage return */
        while (!value.empty() &&
               (value.back() == '\n' || value.back() == '\r')) {
            value.pop_back();
        }

        Serial.print("Received: ");
        Serial.println(value.c_str());

        /* ── ON:RRGGBB ── */
        if (value.rfind("ON:", 0) == 0 && value.length() == 9) {
            std::string hex = value.substr(3);
            uint8_t r = strtol(hex.substr(0, 2).c_str(), nullptr, 16);
            uint8_t g = strtol(hex.substr(2, 2).c_str(), nullptr, 16);
            uint8_t b = strtol(hex.substr(4, 2).c_str(), nullptr, 16);

            ledSetAll(r, g, b);

            portENTER_CRITICAL(&stateMux);
            ledActive   = true;
            ledOnTimeMs = millis();
            portEXIT_CRITICAL(&stateMux);

            Serial.printf("LED ON r=%d g=%d b=%d\n", r, g, b);

        /* ── OFF ── */
        } else if (value == "OFF") {
            ledOff();
            portENTER_CRITICAL(&stateMux);
            ledActive = false;
            portEXIT_CRITICAL(&stateMux);
            Serial.println("LED OFF");

        /* ── TIMEOUT:xxxxx ── */
        } else if (value.rfind("TIMEOUT:", 0) == 0) {
            unsigned long ms = strtoul(
                value.substr(8).c_str(), nullptr, 10);
            if (ms > 0) {
                portENTER_CRITICAL(&stateMux);
                timeoutMs = ms;
                portEXIT_CRITICAL(&stateMux);
                Serial.print("Timeout set: ");
                Serial.print(ms);
                Serial.println(" ms");
            }

        /* ── Unknown ── */
        } else {
            Serial.print("Unknown command: ");
            Serial.println(value.c_str());
        }
    }
};

/* ══════════════════════════════════════════════════════════
 * IR task — polls sensor, sends RT or TIMEOUT to nRF
 * ══════════════════════════════════════════════════════════ */
void irTask(void *param)
{
    bool prevDetected = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(IR_POLL_MS));

        portENTER_CRITICAL(&stateMux);
        bool          isActive = ledActive;
        unsigned long start    = ledOnTimeMs;
        unsigned long t_out    = timeoutMs;
        portEXIT_CRITICAL(&stateMux);

        if (!isActive) {
            prevDetected = false;
            continue;
        }

        unsigned long elapsed = millis() - start;

        /* ── Timeout check ── */
        if (elapsed >= t_out) {
            ledOff();
            portENTER_CRITICAL(&stateMux);
            ledActive = false;
            portEXIT_CRITICAL(&stateMux);

            char msg[32];
            snprintf(msg, sizeof(msg),
                     "POD%d:TIMEOUT\n", POD_NUMBER);
            sendToNrf(msg);
            Serial.println(msg);
            prevDetected = false;
            continue;
        }

        /* ── IR check ── */
        bool detected = (digitalRead(IR_PIN) == LOW);

        if (!detected || prevDetected) {
            prevDetected = detected;
            continue;
        }
        prevDetected = detected;

        /* Rising edge — hand detected */
        ledOff();
        portENTER_CRITICAL(&stateMux);
        ledActive = false;
        portEXIT_CRITICAL(&stateMux);

        char msg[32];
        snprintf(msg, sizeof(msg),
                 "POD%d:RT:%lu\n", POD_NUMBER, elapsed);
        sendToNrf(msg);

        Serial.print("RT: ");
        Serial.print(elapsed);
        Serial.println(" ms");
    }
}

/* ══════════════════════════════════════════════════════════
 * setup()
 * ══════════════════════════════════════════════════════════ */
void setup()
{
    Serial.begin(115200);
    delay(1000);

    REG_WRITE(RTC_CNTL_BROWN_OUT_REG, 0);

    strip.begin();
    strip.setBrightness(255);
    strip.show();

    pinMode(IR_PIN, INPUT_PULLUP);

    BLEDevice::init(POD_NAME);

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService *pService = pServer->createService(NUS_SERVICE_UUID);

    /* TX — notify nRF */
    pTxChar = pService->createCharacteristic(
        NUS_TX_UUID,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxChar->addDescriptor(new BLE2902());

    /* RX — receive commands from nRF */
    BLECharacteristic *pRxChar = pService->createCharacteristic(
        NUS_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_WRITE_NR
    );
    pRxChar->setCallbacks(new RxCallbacks());

    pService->start();

    /* Advertising */
    BLEAdvertising *pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID(NUS_SERVICE_UUID);
    pAdv->setScanResponse(true);
    BLEDevice::startAdvertising();

    Serial.print(POD_NAME);
    Serial.println(" ready — waiting for nRF");

    xTaskCreatePinnedToCore(
        irTask, "ir_task", 2048,
        nullptr, 1, &irTaskHandle, 1
    );
}

/* ══════════════════════════════════════════════════════════
 * loop()
 * ══════════════════════════════════════════════════════════ */
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
