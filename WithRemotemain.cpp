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
#define REMOTE_MAC "AA:BB:CC:DD:EE:FF"  // Deine SmartRemote

// ==========================================
// --- 2. FAHR-EINSTELLUNGEN ---
// ==========================================
#define SPEED_GEAR_1 6  
#define SPEED_GEAR_2 10 
#define SPEED_GEAR_3 22 

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
static NimBLEUUID hidServiceUUID("1812");

NimBLEClient* pScooterClient = nullptr;
NimBLEClient* pRemoteClient = nullptr;
NimBLERemoteCharacteristic* pChAuthTx = nullptr;
NimBLERemoteCharacteristic* pChDataTx = nullptr;

// --- STATUS VARIABLEN ---
bool isScooterConnected = false;
bool isScooterAuthenticated = false;
bool isRemoteConnected = false;

unsigned long lastScooterReconnect = 0;
unsigned long lastRemoteReconnect = 0;
unsigned long lastCommandTime = 0; 

// --- LOKALER SCOOTER STATUS ---
bool scooterLocked = false; 
bool lightOn = false; 
bool zeroStartEnabled = true;      
uint8_t currentBattery = 0;

// Logik-Speicher für Licht & Gänge
bool lightStateBeforeLock = false; 
uint8_t currentGear = 3; 
uint8_t lastActiveGear = 3; 

// ==========================================
// DISPLAY UPDATE (HUD STYLE)
// ==========================================
void updateDisplay() {
    display.clearDisplay();
    display.setCursor(0, 0);
    
    display.print("F: ");
    if (currentGear == 1) display.print("G");
    else if (currentGear == 2) display.print("D");
    else display.print("S");
    
    display.print(" Z: ");
    display.println(zeroStartEnabled ? "Y" : "N");
    
    display.print("B: ");
    if (currentBattery == 0) display.println("--%"); 
    else { display.print(currentBattery); display.println("%"); }
    
    display.print("S: ");
    display.print(scooterLocked ? "Y" : "N");
    display.print(" L: ");
    display.println(lightOn ? "Y" : "N");

    display.print(isScooterAuthenticated ? "S:OK " : (isScooterConnected ? "S:-- " : "S:XX "));
    display.println(isRemoteConnected ? "R:OK" : "R:XX");

    display.display();
}

// ==========================================
// MODBUS CRC16 BERECHNUNG
// ==========================================
uint16_t calculateModbusCRC(uint8_t *data, uint8_t len) {
    uint16_t crc = 0xFFFF;
    for (uint8_t pos = 0; pos < len; pos++) {
        crc ^= (uint16_t)data[pos];
        for (int i = 8; i != 0; i--) {
            if ((crc & 0x0001) != 0) { crc >>= 1; crc ^= 0xA001; } 
            else { crc >>= 1; }
        }
    }
    return crc;
}

// ==========================================
// BEFEHL AN DEN SCOOTER SENDEN
// ==========================================
void sendScooterCommand() {
    if (!isScooterAuthenticated || pChDataTx == nullptr) return;

    lastCommandTime = millis(); 

    uint8_t configByte = 0x00; 
    
    if (currentGear == 2) configByte |= 0x01;
    else if (currentGear == 3) configByte |= 0x02;

    if (!scooterLocked) configByte |= 0x80;                 
    if (lightOn) configByte |= 0x04;                        
    if (zeroStartEnabled) configByte |= ZERO_START_BIT;     

    uint8_t packet[10] = {
        0xAF, 0x00, 0x0A, 
        configByte, 0x03, 
        SPEED_GEAR_1, SPEED_GEAR_2, SPEED_GEAR_3, 
        0x00, 0x00
    };

    uint16_t crc = calculateModbusCRC(packet, 8);
    packet[8] = (crc & 0xFF);        
    packet[9] = (crc >> 8) & 0xFF;   

    pChDataTx->writeValue(packet, 10, false);
    updateDisplay();
}

// ==========================================
// CALLBACKS: SCOOTER EMPFANG
// ==========================================
void authNotifyCallback(NimBLERemoteCharacteristic* pCh, uint8_t* pData, size_t length, bool isNotify) {
    String response = "";
    for (int i = 0; i < length; i++) response += (char)pData[i];
    
    if (response.indexOf("OK+PWD:Y") >= 0) {
        isScooterAuthenticated = true;
        sendScooterCommand(); 
    }
    updateDisplay();
}

void scooterDataCallback(NimBLERemoteCharacteristic* pCh, uint8_t* pData, size_t length, bool isNotify) {
    if (millis() - lastCommandTime < 1500) return; 

    if (length >= 25 && pData[0] == 0xAF && pData[1] == 0x00) {
        currentBattery = pData[5];
        
        bool newLockState = !(pData[21] & 0x08);
        bool newLightState = (pData[22] & 0x04);
        
        if (newLockState != scooterLocked) {
            if (newLockState) {
                lightStateBeforeLock = lightOn;
                preferences.putBool("l_mem", lightStateBeforeLock);
                lightOn = false;
            } else {
                lightOn = lightStateBeforeLock;
            }
            scooterLocked = newLockState;
            preferences.putBool("lock", scooterLocked);
        }

        if (!scooterLocked) {
            lightOn = newLightState;
        }
        
        preferences.putUChar("bat", currentBattery);
        preferences.putBool("light", lightOn);
        updateDisplay();
    }
}

// ==========================================
// CALLBACKS: FERNBEDIENUNG EMPFANG
// ==========================================
void remoteNotifyCallback(NimBLERemoteCharacteristic* pCh, uint8_t* pData, size_t length, bool isNotify) {
    if (length == 2 && pData[1] == 0x00) {
        
        // --- 0x04 = Vorheriger Titel (Einfachklick) -> Lock/Unlock ---
        if (pData[0] == 0x04) {
            scooterLocked = !scooterLocked;
            preferences.putBool("lock", scooterLocked); // SOFORT SPEICHERN
            
            if (scooterLocked) {
                lightStateBeforeLock = lightOn; 
                preferences.putBool("l_mem", lightStateBeforeLock); 
                lightOn = false;                
            } else {
                lightOn = lightStateBeforeLock; 
            }
            preferences.putBool("light", lightOn); // SOFORT SPEICHERN
            sendScooterCommand();
        }
        
        // --- 0x10 = Play/Pause (Einfachklick) -> Licht ---
        else if (pData[0] == 0x10) {
            if (!scooterLocked) {
                lightOn = !lightOn;
                preferences.putBool("light", lightOn); // SOFORT SPEICHERN
                sendScooterCommand();
            } else {
                lightStateBeforeLock = !lightStateBeforeLock;
                preferences.putBool("l_mem", lightStateBeforeLock); // SOFORT SPEICHERN
                updateDisplay(); 
            }
        }
        
        // --- 0x08 = Nächster Titel (Einfachklick) -> Gangwechsel / Zurück ---
        else if (pData[0] == 0x08) {
            if (currentGear == 1) {
                currentGear = lastActiveGear; 
            } else {
                currentGear = (currentGear == 2) ? 3 : 2;
                lastActiveGear = currentGear; 
            }
            preferences.putUChar("gear", currentGear); // SOFORT SPEICHERN
            preferences.putUChar("l_gear", lastActiveGear); // SOFORT SPEICHERN
            sendScooterCommand();
        }
        
        // --- 0x40 = Nächster Titel (Doppelklick) -> Geh Modus (1) ---
        else if (pData[0] == 0x40) {
            if (currentGear != 1) {
                lastActiveGear = currentGear; 
                preferences.putUChar("l_gear", lastActiveGear); // SOFORT SPEICHERN
            }
            currentGear = 1;
            preferences.putUChar("gear", currentGear); // SOFORT SPEICHERN
            sendScooterCommand();
        }
        
        // --- 0x80 = Vorheriger Titel (Doppelklick) -> Zero-Start ---
        else if (pData[0] == 0x80) {
            zeroStartEnabled = !zeroStartEnabled;
            preferences.putBool("zero", zeroStartEnabled); // SOFORT SPEICHERN
            sendScooterCommand();
        }
    }
}

// ==========================================
// BLE VERBINDUNGS-MANAGEMENT
// ==========================================
class ScooterClientCallback : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient* pclient) override {
        isScooterConnected = false;
        isScooterAuthenticated = false;
        updateDisplay();
    }
};

class RemoteClientCallback : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient* pclient) override {
        isRemoteConnected = false;
        updateDisplay();
    }
};

class MySecurity : public NimBLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t pass_key) override {}
    bool onConfirmPIN(uint32_t pass_key) override { return true; }
    bool onSecurityRequest() override { return true; }
    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {}
};

void connectToScooter() {
    if (pScooterClient == nullptr) {
        pScooterClient = NimBLEDevice::createClient();
        pScooterClient->setClientCallbacks(new ScooterClientCallback(), false);
        pScooterClient->setConnectTimeout(3); 
    }
    
    if (pScooterClient->connect(NimBLEAddress(SCOOTER_MAC))) {
        NimBLERemoteService* pAuthService = pScooterClient->getService(authServiceUUID);
        if (pAuthService) {
            pChAuthTx = pAuthService->getCharacteristic(authTxUUID);
            auto pRx = pAuthService->getCharacteristic(authRxUUID);
            if (pRx && pRx->canNotify()) pRx->subscribe(true, authNotifyCallback);
        }

        NimBLERemoteService* pDataService = pScooterClient->getService(dataServiceUUID);
        if (pDataService) {
            pChDataTx = pDataService->getCharacteristic(dataTxUUID);
            auto pRx = pDataService->getCharacteristic(dataRxUUID);
            if (pRx && pRx->canNotify()) pRx->subscribe(true, scooterDataCallback);
        }

        isScooterConnected = true;
        updateDisplay();

        if (pChAuthTx) {
            String authCmd = "AT+PWD[" + String(PIN_CODE) + "]";
            pChAuthTx->writeValue((const uint8_t*)authCmd.c_str(), authCmd.length(), false);
        }
    }
}

void connectToRemote() {
    if (pRemoteClient == nullptr) {
        pRemoteClient = NimBLEDevice::createClient();
        pRemoteClient->setClientCallbacks(new RemoteClientCallback(), false);
        pRemoteClient->setConnectTimeout(3); 
    }
    
    if (pRemoteClient->connect(NimBLEAddress(REMOTE_MAC))) {
        pRemoteClient->secureConnection();
        delay(1000); 

        NimBLERemoteService* pHidService = pRemoteClient->getService(hidServiceUUID);
        if (pHidService) {
            auto chars = pHidService->getCharacteristics(true);
            if(chars) {
                for (auto pCh : *chars) {
                    NimBLEUUID uuid = pCh->getUUID();
                    if (uuid.equals(NimBLEUUID((uint16_t)0x2a4e))) {
                        uint8_t mode = 0x01; pCh->writeValue(&mode, 1, true); delay(50);
                    } else if (uuid.equals(NimBLEUUID((uint16_t)0x2a4c))) {
                        uint8_t wakeup = 0x01; pCh->writeValue(&wakeup, 1, false); delay(50);
                    } else if (pCh->canNotify()) {
                        pCh->subscribe(true, remoteNotifyCallback, true); delay(50);
                    }
                }
            }
            isRemoteConnected = true;
            updateDisplay();
        } else {
            pRemoteClient->disconnect();
        }
    }
}

// ==========================================
// SETUP
// ==========================================
void setup() {
    Serial.begin(115200);
    
    // ALLE gespeicherten Einstellungen aus dem Flash-Speicher laden
    preferences.begin("epf", false);
    scooterLocked = preferences.getBool("lock", false);
    lightOn = preferences.getBool("light", false);
    zeroStartEnabled = preferences.getBool("zero", true);
    currentBattery = preferences.getUChar("bat", 0);
    
    currentGear = preferences.getUChar("gear", 3);       // Standard: Sport (3)
    lastActiveGear = preferences.getUChar("l_gear", 3);  // Standard: Sport (3)
    
    lightStateBeforeLock = preferences.getBool("l_mem", lightOn);

    pinMode(VEXT_PIN, OUTPUT);
    digitalWrite(VEXT_PIN, LOW); 
    delay(50); 

    Wire.begin(OLED_SDA, OLED_SCL);
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) for(;;);
    
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    updateDisplay();
    
    NimBLEDevice::init("");
    NimBLEDevice::setSecurityAuth(true, true, true); 
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityCallbacks(new MySecurity());
    
    connectToScooter();
    connectToRemote();
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
    if (!isScooterConnected && (millis() - lastScooterReconnect > 3000)) { 
        lastScooterReconnect = millis();
        connectToScooter();
    }
    
    if (!isRemoteConnected && (millis() - lastRemoteReconnect > 5000)) { 
        lastRemoteReconnect = millis();
        connectToRemote();
    }
    
    delay(100);
}
