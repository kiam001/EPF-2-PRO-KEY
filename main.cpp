#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// ==========================================
// --- 1. VERBINDUNGS-DATEN ---
// ==========================================
#define SCOOTER_MAC "AA:BB:CC:DD:EE:FF" // MAC-Adresse deines ePF-Scooters
#define PIN_CODE "888888"               // 6-stelliger App-Code (Default: 888888)
#define BUTTON_PIN 0                    // BOOT/PRG Button beim ESP32-S3

// ==========================================
// --- 2. FAHR-EINSTELLUNGEN ---
// ==========================================
#define SPEED_GEAR_1 6  
#define SPEED_GEAR_2 15 
#define SPEED_GEAR_3 22 

// ZERO-START (Bit 5 im configByte aktiviert den Direktstart aus dem Stand)
#define ZERO_START_BIT (1 << 5)     

// --- PINS (Heltec Wireless Stick V3) ---
#define VEXT_PIN 36
#define OLED_SDA 17
#define OLED_SCL 18
#define OLED_RST 21

Adafruit_SSD1306 display(64, 32, &Wire, OLED_RST);
Preferences preferences;

// --- UUIDs ---
static NimBLEUUID authServiceUUID("F2F0");
static NimBLEUUID authTxUUID("F2F1");
static NimBLEUUID authRxUUID("F2F2");
static NimBLEUUID dataServiceUUID("F1F0");
static NimBLEUUID dataTxUUID("F1F1");
static NimBLEUUID dataRxUUID("F1F2");

// --- BLE VARIABLEN ---
NimBLEClient* pClient = nullptr;
NimBLERemoteCharacteristic* pChAuthTx = nullptr;
NimBLERemoteCharacteristic* pChAuthRx = nullptr;
NimBLERemoteCharacteristic* pChDataTx = nullptr;
NimBLERemoteCharacteristic* pChDataRx = nullptr;

bool isConnected = false;
bool isAuthenticated = false;

// --- LOKALER STATUS ---
bool scooterLocked = false; 
bool lightOn = false;       
uint8_t currentBattery = 0;

// --- BUTTON LOGIK ---
int lastButtonState = HIGH;
unsigned long buttonPressTime = 0;
bool isPressing = false;
const unsigned long longPressThreshold = 600; 

unsigned long lastReconnectAttempt = 0;

// ==========================================
// DISPLAY UPDATE
// ==========================================
void updateDisplay() {
    display.clearDisplay();
    display.setCursor(0, 0);
    
    if (!isConnected) {
        display.println("Suche...");
    } else if (!isAuthenticated) {
        display.println("Auth...");
    } else {
        display.println("Verbunden");
        
        display.print("Akku: ");
        if (currentBattery == 0) {
            display.println("--%"); 
        } else if (currentBattery >= 100) {
            display.println("FU");
        } else {
            display.print(currentBattery);
            display.println("%");
        }
        
        display.print("Licht: ");
        display.println(lightOn ? "AN" : "AUS");
        
        display.print("Lock: ");
        display.println(scooterLocked ? "JA" : "NEIN");
    }

    display.display();
}

// ==========================================
// CALLBACK FÜR VERBINDUNGSABBRÜCHE
// ==========================================
class MyClientCallback : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient* pclient) override {
        Serial.println("Verbindung getrennt!");
        isConnected = false;
        isAuthenticated = false;
        updateDisplay();
    }
};

// ==========================================
// MODBUS CRC16 BERECHNUNG (Big-Endian)
// ==========================================
uint16_t calculateModbusCRC(uint8_t *data, uint8_t len) {
    uint16_t crc = 0xFFFF;
    for (uint8_t pos = 0; pos < len; pos++) {
        crc ^= (uint16_t)data[pos];
        for (int i = 8; i != 0; i--) {
            if ((crc & 0x0001) != 0) {
                crc >>= 1; crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// ==========================================
// STEUERBEFEHL SENDEN
// ==========================================
void sendControlCommand() {
    if (!isAuthenticated || pChDataTx == nullptr) return;

    // Basis: 3. Gang (Bit 0 & 1)
    uint8_t configByte = 0x02; 
    
    if (!scooterLocked) configByte |= 0x80; // Bit 7: Lock
    if (lightOn) configByte |= 0x04;        // Bit 2: Licht
    configByte |= ZERO_START_BIT;           // Bit 5: Zero-Start

    // Paket zusammenbauen
    uint8_t packet[10] = {
        0xAF, 
        0x00, 
        0x0A, 
        configByte, 
        0x03, 
        SPEED_GEAR_1, 
        SPEED_GEAR_2, 
        SPEED_GEAR_3, 
        0x00, 
        0x00
    };

    uint16_t crc = calculateModbusCRC(packet, 8);
    packet[8] = (crc & 0xFF);        
    packet[9] = (crc >> 8) & 0xFF;   

    pChDataTx->writeValue(packet, 10, false);
}

// ==========================================
// BLE CALLBACKS (EMPFANG)
// ==========================================
void authNotifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    String response = "";
    for (int i = 0; i < length; i++) response += (char)pData[i];
    
    if (response.indexOf("OK+PWD:Y") >= 0) {
        Serial.println("Passwort akzeptiert!");
        isAuthenticated = true;
    }
    updateDisplay();
}

void dataNotifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    if (length >= 25 && pData[0] == 0xAF && pData[1] == 0x00) {
        
        currentBattery = pData[5];
        
        scooterLocked = !(pData[21] & 0x08);
        lightOn = (pData[22] & 0x04);
        
        preferences.putUChar("bat", currentBattery);
        preferences.putBool("lock", scooterLocked);
        preferences.putBool("light", lightOn);

        updateDisplay();
    }
}

// ==========================================
// VERBINDUNGSAUFBAU
// ==========================================
bool connectToScooter() {
    Serial.println("Versuche zu verbinden...");
    NimBLEAddress scooterAddress(SCOOTER_MAC);
    
    if (pClient == nullptr) {
        if(NimBLEDevice::getClientListSize() >= NIMBLE_MAX_CONNECTIONS) return false;
        pClient = NimBLEDevice::createClient();
        pClient->setClientCallbacks(new MyClientCallback(), false);
        pClient->setConnectTimeout(2); 
    }
    
    if (!pClient->connect(scooterAddress)) {
        return false;
    }
    
    NimBLERemoteService* pAuthService = pClient->getService(authServiceUUID);
    if (pAuthService != nullptr) {
        pChAuthTx = pAuthService->getCharacteristic(authTxUUID);
        pChAuthRx = pAuthService->getCharacteristic(authRxUUID);
        if (pChAuthRx && pChAuthRx->canNotify()) pChAuthRx->subscribe(true, authNotifyCallback);
    }

    NimBLERemoteService* pDataService = pClient->getService(dataServiceUUID);
    if (pDataService != nullptr) {
        pChDataTx = pDataService->getCharacteristic(dataTxUUID);
        pChDataRx = pDataService->getCharacteristic(dataRxUUID);
        if (pChDataRx && pChDataRx->canNotify()) pChDataRx->subscribe(true, dataNotifyCallback);
    }

    isConnected = true;
    updateDisplay();

    if (pChAuthTx != nullptr) {
        String authCmd = "AT+PWD[" + String(PIN_CODE) + "]";
        pChAuthTx->writeValue((const uint8_t*)authCmd.c_str(), authCmd.length(), false);
    }
    return true;
}

// ==========================================
// SETUP
// ==========================================
void setup() {
    Serial.begin(115200);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    preferences.begin("epf", false);
    
    scooterLocked = preferences.getBool("lock", false);
    lightOn = preferences.getBool("light", false);
    currentBattery = preferences.getUChar("bat", 0);

    pinMode(VEXT_PIN, OUTPUT);
    digitalWrite(VEXT_PIN, LOW); 
    delay(50); 

    Wire.begin(OLED_SDA, OLED_SCL);
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) for(;;);
    
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    updateDisplay();
    
    NimBLEDevice::init("");
    connectToScooter();
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
    if (!isConnected) {
        if (millis() - lastReconnectAttempt > 3000) { 
            lastReconnectAttempt = millis();
            connectToScooter();
        }
    }
    
    int currentState = digitalRead(BUTTON_PIN);
    
    if (currentState == LOW && lastButtonState == HIGH) {
        buttonPressTime = millis();
        isPressing = true;
        delay(50); 
    } 
    else if (currentState == HIGH && lastButtonState == LOW) {
        if (isPressing) {
            unsigned long pressDuration = millis() - buttonPressTime;
            isPressing = false;
            
            if (isConnected && isAuthenticated) {
                if (pressDuration >= longPressThreshold) {
                    lightOn = !lightOn;
                    sendControlCommand();
                } else if (pressDuration > 50) { 
                    scooterLocked = !scooterLocked;
                    sendControlCommand();
                }
            }
        }
        delay(50);
    }
    lastButtonState = currentState;
}
