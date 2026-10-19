#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// ==========================================
// --- 1. VERBINDUNGS-DATEN ---
// ==========================================
#define SCOOTER_MAC "AA:BB:CC:DD:EE:FF" 
#define PIN_CODE "888888"               
#define REMOTE1_MAC "AA:BB:CC:DD:EE:FF" // Alte Fernbedienung (Links)
#define REMOTE2_MAC "AA:BB:CC:DD:EE:FF" // NEUE Fernbedienung (Rechts)


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

// --- BLE CLIENTS ---
NimBLEClient* pScooterClient = nullptr;
NimBLEClient* pRemote1Client = nullptr;
NimBLEClient* pRemote2Client = nullptr;

NimBLERemoteCharacteristic* volatile pChAuthTx = nullptr;
NimBLERemoteCharacteristic* volatile pChDataTx = nullptr;

// --- STATUS VARIABLEN (volatile für Thread-Sicherheit) ---
volatile bool isScooterConnected = false;
volatile bool isScooterAuthenticated = false;
volatile bool isRemote1Connected = false;
volatile bool isRemote2Connected = false;

// --- THREAD-SAFE FLAGS FÜR DIE HAUPTSCHLEIFE ---
volatile bool flagUpdateDisplay = false;
volatile bool flagSendCommand = false;
volatile bool flagSavePrefs = false;

unsigned long lastScooterReconnect = 0;
unsigned long lastRemote1Reconnect = 0;
unsigned long lastRemote2Reconnect = 0;
volatile unsigned long lastCommandTime = 0; 

// --- LOKALER SCOOTER STATUS ---
volatile bool scooterLocked = false; 
volatile bool lightOn = false; 
volatile bool zeroStartEnabled = true;      
volatile uint8_t currentBattery = 0;

// Logik-Speicher für Licht & Gänge
volatile bool lightStateBeforeLock = false; 
volatile uint8_t currentGear = 3; 
volatile uint8_t lastActiveGear = 3; 

// ==========================================
// DISPLAY UPDATE (Wird nur noch sicher in loop() aufgerufen)
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

    // Ultra kompakte Anzeige für 3 Verbindungen
    display.print(isScooterAuthenticated ? "S:Y R:" : (isScooterConnected ? "S:- R:" : "S:N R:"));
    display.print(isRemote1Connected ? "1" : "-");
    display.print(",");
    display.println(isRemote2Connected ? "2" : "-");

    display.display();
}

// ==========================================
// PREFERENCES SPEICHERN (Sicher außerhalb von BLE-Callbacks)
// ==========================================
void savePreferences() {
    preferences.putBool("lock", scooterLocked);
    preferences.putBool("light", lightOn);
    preferences.putBool("l_mem", lightStateBeforeLock);
    preferences.putBool("zero", zeroStartEnabled);
    preferences.putUChar("bat", currentBattery);
    preferences.putUChar("gear", currentGear);
    preferences.putUChar("l_gear", lastActiveGear);
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
    if (!isScooterConnected || !isScooterAuthenticated || pChDataTx == nullptr) return;
    if (pScooterClient == nullptr || !pScooterClient->isConnected()) return;

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
    flagUpdateDisplay = true;
}

// ==========================================
// CALLBACKS: SCOOTER EMPFANG
// ==========================================
void authNotifyCallback(NimBLERemoteCharacteristic* pCh, uint8_t* pData, size_t length, bool isNotify) {
    String response = "";
    for (size_t i = 0; i < length; i++) response += (char)pData[i];
    
    if (response.indexOf("OK+PWD:Y") >= 0) {
        isScooterAuthenticated = true;
        flagSendCommand = true; 
    }
    flagUpdateDisplay = true;
}

void scooterDataCallback(NimBLERemoteCharacteristic* pCh, uint8_t* pData, size_t length, bool isNotify) {
    if (millis() - lastCommandTime < 1500) return; 

    if (length >= 25 && pData[0] == 0xAF && pData[1] == 0x00) {
        bool stateChanged = false;

        uint8_t newBat = pData[5];
        if (newBat != currentBattery) {
            currentBattery = newBat;
            stateChanged = true;
        }
        
        bool newLockState = !(pData[21] & 0x08);
        bool newLightState = (pData[22] & 0x04);
        
        if (newLockState != scooterLocked) {
            if (newLockState) {
                lightStateBeforeLock = lightOn;
                lightOn = false;
            } else {
                lightOn = lightStateBeforeLock;
            }
            scooterLocked = newLockState;
            stateChanged = true;
        }

        if (!scooterLocked && (lightOn != newLightState)) {
            lightOn = newLightState;
            stateChanged = true;
        }
        
        if (stateChanged) {
            flagSavePrefs = true;
            flagUpdateDisplay = true;
        }
    }
}

// ==========================================
// CALLBACKS: FERNBEDIENUNG 1 (SCHLÜSSEL LINKS)
// ==========================================
void remote1NotifyCallback(NimBLERemoteCharacteristic* pCh, uint8_t* pData, size_t length, bool isNotify) {
    if (length == 2 && pData[1] == 0x00) {
        
        // 0x04 = Vorheriger Titel -> Lock/Unlock
        if (pData[0] == 0x04) {
            scooterLocked = !scooterLocked;
            if (scooterLocked) {
                lightStateBeforeLock = lightOn; 
                lightOn = false;                
            } else {
                lightOn = lightStateBeforeLock;
            }
            lastCommandTime = millis();
            flagSavePrefs = true;
            flagSendCommand = true;
            flagUpdateDisplay = true;
        }
        // 0x10 = Play/Pause -> Licht
        else if (pData[0] == 0x10) {
            if (!scooterLocked) {
                lightOn = !lightOn;
                lastCommandTime = millis();
                flagSavePrefs = true;
                flagSendCommand = true;
                flagUpdateDisplay = true;
            } else {
                lightStateBeforeLock = !lightStateBeforeLock;
                flagSavePrefs = true;
                flagUpdateDisplay = true; 
            }
        }
        // 0x08 = Nächster Titel -> Gangwechsel (D/S)
        else if (pData[0] == 0x08) {
            if (currentGear == 1) currentGear = lastActiveGear; 
            else { currentGear = (currentGear == 2) ? 3 : 2; lastActiveGear = currentGear; }
            lastCommandTime = millis();
            flagSavePrefs = true;
            flagSendCommand = true;
            flagUpdateDisplay = true;
        }
        // 0x40 = Nächster Titel (Doppelklick) -> Geh Modus
        else if (pData[0] == 0x40) {
            if (currentGear != 1) { lastActiveGear = currentGear; }
            currentGear = 1;
            lastCommandTime = millis();
            flagSavePrefs = true;
            flagSendCommand = true;
            flagUpdateDisplay = true;
        }
        // 0x80 = Vorheriger Titel (Doppelklick) -> Zero-Start
        else if (pData[0] == 0x80) {
            zeroStartEnabled = !zeroStartEnabled;
            lastCommandTime = millis();
            flagSavePrefs = true;
            flagSendCommand = true;
            flagUpdateDisplay = true;
        }
    }
}

// ==========================================
// CALLBACKS: FERNBEDIENUNG 2 (DASHBOARD RECHTS)
// ==========================================
void remote2NotifyCallback(NimBLERemoteCharacteristic* pCh, uint8_t* pData, size_t length, bool isNotify) {
    if (length == 2 && pData[1] == 0x00) {
        
        // 0x40 = Nächster Titel (Doppelklick) -> Licht an/aus
        if (pData[0] == 0x40) {
            if (!scooterLocked) {
                lightOn = !lightOn;
                lastCommandTime = millis();
                flagSavePrefs = true;
                flagSendCommand = true;
                flagUpdateDisplay = true;
            } else {
                lightStateBeforeLock = !lightStateBeforeLock;
                flagSavePrefs = true;
                flagUpdateDisplay = true; 
            }
        }
        // 0x08 = Nächster Titel (Einfachklick) -> Sport Modus (3)
        else if (pData[0] == 0x08) {
            currentGear = 3;
            lastActiveGear = 3; // Speichern für Remote 1
            lastCommandTime = millis();
            flagSavePrefs = true;
            flagSendCommand = true;
            flagUpdateDisplay = true;
        }
        // 0x10 = Play/Pause (Einfachklick) -> Dynamic Modus (2)
        else if (pData[0] == 0x10) {
            currentGear = 2;
            lastActiveGear = 2; // Speichern für Remote 1
            lastCommandTime = millis();
            flagSavePrefs = true;
            flagSendCommand = true;
            flagUpdateDisplay = true;
        }
        // 0x04 = Vorheriger Titel (Einfachklick) -> Eco/Geh Modus (1)
        else if (pData[0] == 0x04) {
            currentGear = 1;
            // lastActiveGear wird NICHT überschrieben
            lastCommandTime = millis();
            flagSavePrefs = true;
            flagSendCommand = true;
            flagUpdateDisplay = true;
        }
        // 0x80 = Vorheriger Titel (Doppelklick) -> Zero-Start
        else if (pData[0] == 0x80) {
            zeroStartEnabled = !zeroStartEnabled;
            lastCommandTime = millis();
            flagSavePrefs = true;
            flagSendCommand = true;
            flagUpdateDisplay = true;
        }
    }
}

// ==========================================
// BLE VERBINDUNGS-MANAGEMENT
// ==========================================
class ScooterClientCallback : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient* pclient) override {
        pChAuthTx = nullptr;
        pChDataTx = nullptr;
        isScooterConnected = false;
        isScooterAuthenticated = false;
        flagUpdateDisplay = true;
    }
};

class Remote1ClientCallback : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient* pclient) override {
        isRemote1Connected = false;
        flagUpdateDisplay = true;
    }
};

class Remote2ClientCallback : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient* pclient) override {
        isRemote2Connected = false;
        flagUpdateDisplay = true;
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
    
    if (pScooterClient->isConnected()) return;

    // Alte Pointer zwingend vor dem Connect-Versuch löschen!
    pChAuthTx = nullptr;
    pChDataTx = nullptr;

    if (pScooterClient->connect(NimBLEAddress(SCOOTER_MAC), true)) {
        if (!pScooterClient->isConnected()) return;

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

        // Prüfen, ob beide wichtigen Characteristics gefunden wurden
        if (pChAuthTx != nullptr && pChDataTx != nullptr && pScooterClient->isConnected()) {
            isScooterConnected = true;
            flagUpdateDisplay = true;

            String authCmd = "AT+PWD[" + String(PIN_CODE) + "]";
            pChAuthTx->writeValue((const uint8_t*)authCmd.c_str(), authCmd.length(), false);
        } else {
            // Halboffene / fehlerhafte Verbindung sauber trennen
            pChAuthTx = nullptr;
            pChDataTx = nullptr;
            pScooterClient->disconnect();
        }
    }
}

void connectToRemote1() {
    if (pRemote1Client == nullptr) {
        pRemote1Client = NimBLEDevice::createClient();
        pRemote1Client->setClientCallbacks(new Remote1ClientCallback(), false);
        pRemote1Client->setConnectTimeout(3); 
    }

    if (pRemote1Client->isConnected()) return;

    if (pRemote1Client->connect(NimBLEAddress(REMOTE1_MAC), true)) {
        pRemote1Client->secureConnection();
        delay(800); 
        
        // Prüfen, ob die Verbindung das Delay überlebt hat
        if (!pRemote1Client->isConnected()) return;

        NimBLERemoteService* pHidService = pRemote1Client->getService(hidServiceUUID);
        if (pHidService && pRemote1Client->isConnected()) {
            auto chars = pHidService->getCharacteristics(true);
            if (chars) {
                for (auto pCh : *chars) {
                    if (!pRemote1Client->isConnected()) break;
                    NimBLEUUID uuid = pCh->getUUID();
                    if (uuid.equals(NimBLEUUID((uint16_t)0x2a4e))) { uint8_t m = 0x01; pCh->writeValue(&m, 1, true); delay(50); } 
                    else if (uuid.equals(NimBLEUUID((uint16_t)0x2a4c))) { uint8_t w = 0x01; pCh->writeValue(&w, 1, false); delay(50); } 
                    else if (pCh->canNotify()) { pCh->subscribe(true, remote1NotifyCallback, true); delay(50); }
                }
            }
            if (pRemote1Client->isConnected()) {
                isRemote1Connected = true;
                flagUpdateDisplay = true;
            }
        } else {
            pRemote1Client->disconnect();
        }
    }
}

void connectToRemote2() {
    if (pRemote2Client == nullptr) {
        pRemote2Client = NimBLEDevice::createClient();
        pRemote2Client->setClientCallbacks(new Remote2ClientCallback(), false);
        pRemote2Client->setConnectTimeout(3); 
    }

    if (pRemote2Client->isConnected()) return;

    if (pRemote2Client->connect(NimBLEAddress(REMOTE2_MAC), true)) {
        pRemote2Client->secureConnection();
        delay(800); 
        
        // Prüfen, ob die Verbindung das Delay überlebt hat
        if (!pRemote2Client->isConnected()) return;

        NimBLERemoteService* pHidService = pRemote2Client->getService(hidServiceUUID);
        if (pHidService && pRemote2Client->isConnected()) {
            auto chars = pHidService->getCharacteristics(true);
            if (chars) {
                for (auto pCh : *chars) {
                    if (!pRemote2Client->isConnected()) break;
                    NimBLEUUID uuid = pCh->getUUID();
                    if (uuid.equals(NimBLEUUID((uint16_t)0x2a4e))) { uint8_t m = 0x01; pCh->writeValue(&m, 1, true); delay(50); } 
                    else if (uuid.equals(NimBLEUUID((uint16_t)0x2a4c))) { uint8_t w = 0x01; pCh->writeValue(&w, 1, false); delay(50); } 
                    else if (pCh->canNotify()) { pCh->subscribe(true, remote2NotifyCallback, true); delay(50); }
                }
            }
            if (pRemote2Client->isConnected()) {
                isRemote2Connected = true;
                flagUpdateDisplay = true;
            }
        } else {
            pRemote2Client->disconnect();
        }
    }
}

// ==========================================
// SETUP
// ==========================================
void setup() {
    Serial.begin(115200);
    
    preferences.begin("epf", false);
    scooterLocked = preferences.getBool("lock", false);
    lightOn = preferences.getBool("light", false);
    zeroStartEnabled = preferences.getBool("zero", true);
    currentBattery = preferences.getUChar("bat", 0);
    
    currentGear = preferences.getUChar("gear", 3);       
    lastActiveGear = preferences.getUChar("l_gear", 3);  
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
    
    // Erste Verbindungsversuche beim Start
    connectToScooter();
    lastScooterReconnect = millis();
    
    connectToRemote1();
    lastRemote1Reconnect = millis();
    
    connectToRemote2();
    lastRemote2Reconnect = millis();
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
    // 1. Anstehende Scooter-Befehle sicher im Haupt-Thread ausführen
    if (flagSendCommand) {
        flagSendCommand = false;
        sendScooterCommand();
    }

    // 2. Geänderte Einstellungen im NVS speichern
    if (flagSavePrefs) {
        flagSavePrefs = false;
        savePreferences();
    }

    // 3. Display sicher aktualisieren (verhindert I2C-Crashes)
    if (flagUpdateDisplay) {
        flagUpdateDisplay = false;
        updateDisplay();
    }

    // 4. Reconnects einzeln pro Schleifendurchlauf abarbeiten,
    // damit Befehle und Display-Updates nicht über Sekunden blockiert werden
    if (!isScooterConnected && (millis() - lastScooterReconnect > 3000)) { 
        lastScooterReconnect = millis();
        connectToScooter();
    } 
    else if (!isRemote1Connected && (millis() - lastRemote1Reconnect > 5000)) { 
        lastRemote1Reconnect = millis();
        connectToRemote1();
    } 
    else if (!isRemote2Connected && (millis() - lastRemote2Reconnect > 5000)) { 
        lastRemote2Reconnect = millis();
        connectToRemote2();
    }
    
    delay(50);
}
