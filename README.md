# ⚡ MeterMaster ESP32 Node

**v0.3.0** · ESP32 D1 Mini + 64×48 OLED

Ein WLAN-Display-Node für das [MeterMaster](https://github.com/MPunktBPunkt)-Ökosystem. Der ESP32 holt Zählerwerte aus ioBroker und zeigt sie auf einem winzigen OLED-Display an – konfigurierbar über eine eingebaute Web-Oberfläche.

---

## Screenshots

### Dashboard – Aktueller Zählerwert
![Dashboard](docs/screenshots/dashboard.png)

### Einstellungen – ioBroker Verbindung & Zählerauswahl
![Einstellungen](docs/screenshots/einstellungen-tab.png)

### WLAN & Systeminfo
![WLAN](docs/screenshots/wlan-tab.png)

### Alarm
![Alarm](docs/screenshots/alarm-tab.png)

### Carousel – Mehrere Zähler automatisch blättern
![Carousel](docs/screenshots/carousel-tab.png)

### OTA – Firmware-Update über Browser
![OTA](docs/screenshots/ota-tab.png)

### Info & Changelog
![Info](docs/screenshots/info-tab.png)

---

## Hardware

| Komponente | Modell |
|---|---|
| Mikrocontroller | ESP32 D1 Mini |
| Display | 0.66" OLED 64×48, SSD1306, I²C |
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| Rote LED | GPIO 2 (active HIGH) |

---

## Features

- **Echtzeit-Zählerwerte** via ioBroker Simple-API (HTTP)
- **4 OLED-Stile:** Standard, Groß, Minimal, Invertiert
- **Carousel-Modus:** bis zu 5 Zähler automatisch blättern
- **Alarm:** LED-Blinken + OLED-Warnung bei Schwellwertüberschreitung
- **Node-Registrierung:** ESP32 meldet sich automatisch im ioBroker-Adapter an
- **OTA-Update:** Firmware direkt über den Browser einspielen
- **Debug-Tab:** Live-Log, Heap-Anzeige, Neustart-Button
- **WiFiManager:** WLAN-Konfiguration ohne Neukompilieren

---

## Libraries (Arduino Library Manager)

| Library | Autor | Version |
|---|---|---|
| WiFiManager | tzapu | ≥ 2.0.17 |
| Adafruit SSD1306 | Adafruit | ≥ 2.5 |
| Adafruit GFX Library | Adafruit | ≥ 1.11 |
| ArduinoJson | Benoit Blanchon | ≥ 6.21 |

**Board:** WEMOS D1 MINI ESP32 · ESP32 Board-Package 1.0.6

---

## Installation

1. **Arduino IDE** öffnen, Board `WEMOS D1 MINI ESP32` wählen
2. Libraries über den Library Manager installieren (siehe oben)
3. `MeterMaster_ESP32_Node.ino` öffnen und hochladen
4. Beim ersten Start öffnet der ESP32 einen WLAN-Hotspot namens **`MeterMaster-Setup`**
5. Mit dem Hotspot verbinden → Browser öffnet automatisch die Konfigurationsseite
6. WLAN-Zugangsdaten eingeben → ESP32 verbindet sich und ist unter seiner IP erreichbar

---

## Konfiguration

Nach dem Start ist die Web-Oberfläche unter der IP des ESP32 erreichbar (z.B. `http://192.168.178.110`).

**Einstellungen-Tab:**
- ioBroker Host/IP und Port (Standard: `8087` für simple-api)
- Zähler über „Zähler laden" aus ioBroker auswählen oder State-ID manuell eingeben
- Fetch-Intervall, Bezeichnung und Einheit konfigurieren

---

## OTA-Update

**Manuell:** Im OTA-Tab die `.bin`-Datei per Drag & Drop hochladen.

`.bin` erzeugen: Arduino IDE → *Sketch → Exportiere kompilierte Binärdatei*

**GitHub-Check:** Im OTA-Tab auf „Prüfen" klicken – der Node vergleicht seine Firmware-Version mit dem neuesten GitHub-Release und zeigt einen Download-Link an, wenn eine neue Version verfügbar ist.

---

## API-Endpunkte

| Endpunkt | Methode | Beschreibung |
|---|---|---|
| `/api/version` | GET | Firmware-Version, Build-Datum, GitHub-Infos |
| `/api/status` | GET | Aktueller Zählerwert, Verbindungsstatus |
| `/api/settings` | GET/POST | Einstellungen lesen/schreiben |
| `/api/sysinfo` | GET | WLAN, Heap, CPU, Uptime |
| `/api/discover` | GET | Verfügbare Zähler aus ioBroker laden |
| `/api/alarm` | POST | Alarm konfigurieren |
| `/api/carousel` | GET/POST | Carousel-Konfiguration |
| `/api/carousel/next` | GET | Nächsten Zähler anzeigen |
| `/api/nodeinfo` | GET | Node-Infos für ioBroker-Adapter |
| `/api/nodename` | GET | Node-Name setzen |
| `/api/log` | GET | Debug-Log (letzte 30 Einträge) |
| `/api/led` | GET | LED-Status steuern |
| `/api/restart` | GET | ESP32 neu starten |
| `/update` | POST | OTA Firmware-Upload |

---

## Zusammenspiel mit dem ioBroker-Adapter

Der Node registriert sich automatisch im [iobroker.metermaster](https://github.com/MPunktBPunkt/iobroker.metermaster)-Adapter:

- Schreibt beim Start und alle 60 s seine IP, Name und Version in `metermaster.0.nodes.{MAC}.*`
- Liest alle 15 s `metermaster.0.nodes.{MAC}.config` – der Adapter kann damit den angezeigten Zähler fernsteuern

---

## Changelog

| Version | Änderungen |
|---|---|
| v0.3.0 | Debug-Tab, `/api/log`, `/api/restart`, Bugfixes (JSON-Fehler Discover, loadInfo), Coding Rules |
| v0.1.4 | Info-Tab, GitHub OTA-Check, 4 OLED-Stile, Adapter-Version |
| v0.1.3 | Alarm-Tab, Bluetooth-Tab (Platzhalter), Discover im Einstellungen-Tab |
| v0.1.2 | NTP-Zeit, WLAN-Tab, LED-Steuerung, Lila Theme |
| v0.1.1 | ioBroker Discover-Dropdown, OTA-Update, Web-Interface |
| v0.1.0 | Erstveröffentlichung |

---

## Lizenz

MIT – siehe [LICENSE](LICENSE)

Entwickelt als Companion-Hardware für die MeterMaster App & den ioBroker-Adapter.  
Issues & Feature-Requests gerne auf [GitHub](https://github.com/MPunktBPunkt/esp32.MeterMaster)!
