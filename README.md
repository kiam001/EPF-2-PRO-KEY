# ePowerFun ePF-2 ESP32 BLE Controller

Ein ESP32-Projekt zur Steuerung eines ePowerFun ePF-2 Pro E-Scooters (und kompatiblen Modellen) über Bluetooth Low Energy (BLE). Das Projekt ist optimiert für den **Heltec Wireless Stick V3 (ESP32-S3)** mit integriertem 64x32 OLED-Display.

## 🚀 Features
* **Auto-Connect:** Verbindet sich automatisch mit dem Scooter, sobald dieser eingeschaltet und in Reichweite ist.
* **1-Button-Steuerung:** 
  * Kurzer Druck (< 600ms): Scooter sperren / entsperren (Wegfahrsperre)
  * Langer Druck (> 600ms): Licht ein- / ausschalten
* **OLED-Statusanzeige:** Zeigt Verbindungsstatus, aktuellen Akkustand in Prozent, Licht- und Lock-Status an.
* **Memory-Funktion:** Speichert den letzten bekannten Status (Licht, Lock, Akku) im internen Flash-Speicher des ESP32, um nach einem Neustart direkt synchron zu sein.

## 🛠 Hardware
* [Heltec Wireless Stick V3](https://heltec.org/project/wireless-stick-v3/) (ESP32-S3)
* ePowerFun ePF-2 Pro (sollte auch mit anderen Modellen mit Zydtech/HobbyWing Controllern funktionieren)

## 📦 Software & Bibliotheken
Das Projekt wurde für **PlatformIO** geschrieben. Folgende Bibliotheken werden benötigt (siehe `platformio.ini`):
* `h2zero/NimBLE-Arduino` (Deutlich stabiler und ressourcenschonender als die Standard-BLE-Lib)
* `adafruit/Adafruit SSD1306`
* `adafruit/Adafruit GFX Library`

## ⚙️ Konfiguration (WICHTIG!)
Bevor du den Code hochlädst, musst du in der `src/main.cpp` ganz oben deine Daten anpassen:

```cpp
#define SCOOTER_MAC "AA:BB:CC:DD:EE:FF" // Die echte Bluetooth MAC-Adresse deines Scooters eintragen!
#define PIN_CODE "888888"               // Deine 6-stellige App-PIN (Standard ist 888888)

````

🧠 Funktionsweise & Protokoll-Besonderheiten

Der Scooter arbeitet nach einem strikten "Ping-Pong"-Prinzip und sendet keine Telemetrie-Daten von sich aus. Um an den Status zu kommen, muss ein gültiger Befehl an die Characteristic 0000f1f1 gesendet werden. Der Scooter antwortet daraufhin mit einem Burst auf 0000f1f2.

Da der ePF-2 bei jedem Schreib-Befehl zwangsläufig auch die Konfigurations-Bits für Licht und Schloss übernimmt, merkt sich dieser ESP32-Code den letzten Status lokal über die Preferences.h-Bibliothek. So wird verhindert, dass der Scooter beim Verbinden versehentlich gesperrt oder entsperrt wird.

🙏 Danksagung

Ein riesiges Dankeschön geht an Ennar1991 für das detaillierte Reverse-Engineering des Protokolls! Die hervorragende BLE Telemetry Dokumentation (ZydDash) war die absolute Grundlage für dieses Projekt. Ohne diese Vorarbeit wäre die Umsetzung nicht möglich gewesen.

🤖 KI Disclaimer

Ein großer Teil dieses Codes, der Bluetooth-Logik und dieser Dokumentation wurde iterativ mithilfe von Künstlicher Intelligenz (LLM) entwickelt, analysiert und optimiert.

📜 Lizenz

Do Whatever You Want. Viel Spaß beim Basteln!
