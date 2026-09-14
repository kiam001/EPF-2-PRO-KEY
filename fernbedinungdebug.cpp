#include <Arduino.h>
#include <NimBLEDevice.h>

#define REMOTE_MAC "AA:BB:CC:DD:EE:FF" 

static NimBLEUUID hidServiceUUID("1812"); 

NimBLEClient* pClient = nullptr;
bool isConnected = false;
unsigned long lastReconnectAttempt = 0;

class MySecurity : public NimBLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t pass_key) override {}
    bool onConfirmPIN(uint32_t pass_key) override { return true; }
    bool onSecurityRequest() override { return true; }
    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        if (!desc->sec_state.encrypted) {
            Serial.println(">>> Pairing Info: Unverschlüsselt (nicht optimal).");
        } else {
            Serial.println(">>> Pairing & Verschlüsselung ERFOLGREICH abgeschlossen!");
        }
    }
};

class MyClientCallback : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient* pclient) override {
        Serial.println("Verbindung zur Fernbedienung getrennt!");
        isConnected = false;
    }
};

void notifyCallback(NimBLERemoteCharacteristic* pCh, uint8_t* pData, size_t length, bool isNotify) {
    Serial.print("TASTE GEDRUECKT -> Report (Len ");
    Serial.print(length);
    Serial.print("): ");
    
    for(size_t i = 0; i < length; i++) {
        if(pData[i] < 0x10) Serial.print("0");
        Serial.print(pData[i], HEX);
        Serial.print(" ");
    }
    Serial.println();
}

bool connectToRemote() {
    Serial.println("Versuche direkte Verbindung zur Fernbedienung...");
    NimBLEAddress remoteAddress(REMOTE_MAC);
    
    if (pClient == nullptr) {
        if(NimBLEDevice::getClientListSize() >= NIMBLE_MAX_CONNECTIONS) return false;
        pClient = NimBLEDevice::createClient();
        pClient->setClientCallbacks(new MyClientCallback(), false);
        pClient->setConnectTimeout(5); 
    }
    
    if (!pClient->connect(remoteAddress)) {
        Serial.println("Verbindung fehlgeschlagen.");
        return false;
    }
    
    Serial.println("Verbunden! Erzwinge Verschlüsselung (Pairing)...");
    pClient->secureConnection(); 
    delay(1500); 
    
    isConnected = true;
    NimBLERemoteService* pHidService = pClient->getService(hidServiceUUID);
    
    if (pHidService != nullptr) {
        Serial.println("HID-Service gefunden! Konfiguriere...");
        
        auto characteristics = pHidService->getCharacteristics(true);
        if(characteristics != nullptr) {
            for (auto pCh : *characteristics) {
                NimBLEUUID uuid = pCh->getUUID();
                Serial.print("Charakteristik: ");
                Serial.print(uuid.toString().c_str());
                
                // 1. PROTOCOL MODE SETZEN (Wechselt von Boot zu Report)
                if (uuid.equals(NimBLEUUID((uint16_t)0x2a4e))) {
                    Serial.println(" -> Setze auf 'Report Mode' (0x01)...");
                    uint8_t mode = 0x01;
                    pCh->writeValue(&mode, 1, true); 
                    delay(100);
                }
                // 2. WAKE UP BEFEHL (Exit Suspend)
                else if (uuid.equals(NimBLEUUID((uint16_t)0x2a4c))) {
                    Serial.println(" -> Sende WAKE-UP...");
                    uint8_t wakeup = 0x01;
                    pCh->writeValue(&wakeup, 1, false);
                    delay(100);
                }
                // 3. TASTEN ABONNIEREN (Mit 100ms Pause, um den Chip nicht zu überlasten)
                else if (pCh->canNotify()) {
                    Serial.println(" -> Abonniere...");
                    pCh->subscribe(true, notifyCallback, true);
                    delay(100); 
                } else {
                    Serial.println(" -> Ignoriert.");
                }
            }
        }
        Serial.println(">>> FERTIG! Bitte jetzt Tasten drücken. <<<");
    } else {
        Serial.println("FEHLER: Kein HID-Service.");
        pClient->disconnect();
    }
    
    return true;
}

void setup() {
    Serial.begin(115200);
    Serial.println("Starte direkten BLE HID Sniffer...");

    NimBLEDevice::init("");
    NimBLEDevice::deleteAllBonds(); 
    NimBLEDevice::setSecurityAuth(true, true, true); 
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityCallbacks(new MySecurity());
}

void loop() {
    if (!isConnected) {
        if (millis() - lastReconnectAttempt > 5000) { 
            lastReconnectAttempt = millis();
            connectToRemote();
        }
    }
    delay(10);
}
