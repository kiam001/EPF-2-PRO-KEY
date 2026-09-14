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


# 🛴 ePF-2 SmartRemote Controller (ESP32-S3)

Dieses Projekt verwandelt einen ESP32-S3 (Heltec Wireless Stick V3) in eine unsichtbare Brücke zwischen einer handelsüblichen BLE-Multimedia-Fernbedienung ("SmartRemote") und einem ePowerFun ePF-2 E-Scooter (HobbyWing/Zydtech Controller). 

Der ESP32 baut **zwei Bluetooth-Verbindungen gleichzeitig** auf, übersetzt die HID-Tastenanschläge der Fernbedienung in Modbus-Steuerbefehle und sendet sie in Echtzeit an den Scooter. Ein integriertes OLED-Display dient als kompaktes Head-Up-Display (HUD).

---

## ⚠️ Disclaimer (KI & Sicherheit)

**KI-Disclaimer:**  
> Der Code und die Dokumentation in diesem Projekt wurden in iterativer Zusammenarbeit mit einer Künstlichen Intelligenz (KI) entwickelt. Obwohl der Code intensiv auf Funktionstüchtigkeit getestet wurde, können unerwartete Verhaltensweisen, Ineffizienzen oder Bugs nicht vollständig ausgeschlossen werden. 

**Sicherheits- & Haftungsausschluss:**  
> Die Modifikation der Steuerung eines E-Scooters geschieht **ausschließlich auf eigene Gefahr**. Falsche Konfigurationsdaten oder Verbindungsabbrüche können zu unerwartetem Fahrverhalten (z.B. plötzlicher Parkmodus) führen. 
> * Dieses Projekt ist nicht von ePowerFun oder HobbyWing autorisiert.
> * Die Nutzung im öffentlichen Straßenverkehr (StVZO) kann durch solche Modifikationen die Betriebserlaubnis erlöschen lassen.
> * Der Autor übernimmt **keinerlei Haftung** für Personen-, Sach- oder Folgeschäden, die durch die Nutzung dieses Codes entstehen.

---

## ✨ Features

* **Dual-BLE-Master:** Der ESP32 verwaltet gleichzeitig die verschlüsselte Verbindung zur Fernbedienung und die Modbus-Sitzung zum Scooter.
* **Intelligentes Licht-Gedächtnis:** Wird der Scooter gesperrt, schaltet sich das Licht zum Stromsparen aus. Beim Entsperren wird der exakte vorherige Lichtstatus wiederhergestellt.
* **Smarte Gangschaltung:** Ein Doppelklick aktiviert den Geh-Modus (Gear 1). Ein Einfachklick danach springt exakt in den Fahrmodus (Dynamic oder Sport) zurück, den du vorher genutzt hast.
* **Zero-Start Toggle:** Direktstart aus dem Stand per Knopfdruck aktivierbar. Die Einstellung wird im Flash-Speicher des ESP32 (`Preferences`) gesichert und überlebt einen Neustart.
* **HUD-Display:** Kompakte Statusanzeige aller wichtigen Parameter auf dem 64x32 OLED.

---

## 🎮 Tastenbelegung (SmartRemote)

Die Tasten einer handelsüblichen BLE-Fernbedienung (Media-Keys) wurden wie folgt gemappt:

| Taste auf Fernbedienung | Aktion | Beschreibung |
| :--- | :--- | :--- |
| **Play / Pause** *(Klick)* | 💡 **Licht umschalten** | Schaltet das Scooter-Licht An/Aus. Im gesperrten Zustand wird der Status nur für das spätere Entsperren gemerkt. |
| **Vorheriger Titel** *(Klick)* | 🔒 **Sperren / Entsperren** | Aktiviert die elektronische Wegfahrsperre. |
| **Vorheriger Titel** *(Doppelklick)* | 🚀 **Zero-Start (An/Aus)** | Schaltet den Direktstart um (Speicherung im Flash). |
| **Nächster Titel** *(Klick)* | ⚙️ **Gangwechsel (D/S)** | Wechselt zwischen Dynamic (10 km/h) und Sport (22 km/h). Kommt man aus dem Geh-Modus, wird der letzte Modus wiederhergestellt. |
| **Nächster Titel** *(Doppelklick)*| 🚶 **Geh-Modus (Gear 1)**| Erzwingt den Geh-Modus (6 km/h). |

---

## 📺 Das HUD-Display

Das integrierte OLED-Display zeigt alle Live-Daten im folgenden Format an:

```text
F: S  Z: Y     <-- [F]ahrmodus: G(eh)/D(ynamic)/S(port) | [Z]ero-Start: Y(es)/N(o)
B: 85%         <-- [B]attery: Akku in Prozent
S: N  L: Y     <-- [S]perre aktiv: Y/N | [L]icht an: Y/N
S:OK  R:OK     <-- Debug: [S]cooter Verbunden & Auth | [R]emote Verbunden
``
