# ⚡ MeterMaster ESP32 Node

<p align="center">
  <img src="https://img.shields.io/badge/Version-1.5.0-8b5cf6?style=for-the-badge" />
  <img src="https://img.shields.io/badge/Platform-ESP32-blue?style=for-the-badge&logo=arduino" />
  <img src="https://img.shields.io/badge/Display-OLED%2064×48-white?style=for-the-badge" />
  <img src="https://img.shields.io/badge/ioBroker-Simple--API-orange?style=for-the-badge" />
  <img src="https://img.shields.io/badge/License-MIT-green?style=for-the-badge" />
</p>

Ein WLAN-Display-Node für das [MeterMaster](https://github.com/MPunktBPunkt/iobroker.metermaster) Ökosystem. Der ESP32 holt Zählerwerte direkt aus ioBroker, zeigt sie auf einem 0.66" OLED an, registriert sich im ioBroker-Adapter und kann vom Adapter aus ferngesteuert werden.

---

## 📸 Features

- **OLED-Anzeige** – Zählerwert, Label, Einheit, Uhrzeit der Ablesung, Datum, Status-Punkt
- **4 Display-Stile** – Standard, Groß, Minimal, Invertiert
- **Carousel-Modus** – automatisch durch bis zu 5 Zähler blättern
- **ioBroker Node-Registrierung** – ESP32 meldet sich selbst an, ioBroker-Adapter erkennt ihn
- **Fernsteuerung** – Adapter kann OLED-Zähler und Carousel über ioBroker-States steuern
- **Alarm** – LED blinkt bei Schwellwertüberschreitung, OLED zeigt `!ALM`
- **Web-Interface** – 8 Tabs: Dashboard, Einstellungen, WLAN, Alarm, Bluetooth, Carousel, OTA, Info
- **OTA Update** – Firmware per Browser hochladen oder GitHub-Release prüfen
- **NTP-Zeitsync** – Uhrzeit und Datum der letzten Ablesung direkt vom ioBroker-Timestamp
- **LED-Steuerung** – rote LED per Web-Button ein/ausschalten

---

## 🔧 Hardware

| Teil | Beschreibung |
|---|---|
| **Mikrocontroller** | WEMOS D1 Mini ESP32 |
| **Display** | 0.66" OLED, 64×48 Pixel, SSD1306, I²C |
| **Anschluss** | SDA → GPIO21, SCL → GPIO22 |
| **I²C-Adresse** | 0x3C |
| **LED** | GPIO 2 (eingebaut, active HIGH) |

### Schaltplan

```
WEMOS D1 Mini ESP32
        │
  GPIO21 (SDA) ──────────────── OLED SDA
  GPIO22 (SCL) ──────────────── OLED SCL
  3.3V         ──────────────── OLED VCC
  GND          ──────────────── OLED GND
```

---

## 📦 Installation

### 1. Arduino IDE vorbereiten

**Board hinzufügen** (falls noch nicht vorhanden):  
`Datei → Voreinstellungen → Zusätzliche Boardverwalter-URLs:`
```
https://dl.espressif.com/dl/package_esp32_index.json
```
Dann: `Werkzeuge → Board → Boardverwalter → "esp32" installieren`

**Board auswählen:**  
`Werkzeuge → Board → WEMOS D1 MINI ESP32`

**Einstellungen:**
```
CPU Frequency:  240MHz (WiFi/BT)
Flash Size:     4MB (Default)
Upload Speed:   921600
```

### 2. Libraries installieren

`Sketch → Bibliothek einbinden → Bibliotheken verwalten`

| Library | Autor | Version |
|---|---|---|
| WiFiManager | tzapu | ≥ 2.0.17 |
| Adafruit SSD1306 | Adafruit | ≥ 2.5 |
| Adafruit GFX Library | Adafruit | ≥ 1.11 |
| ArduinoJson | Benoit Blanchon | ≥ 6.21 |

### 3. Sketch flashen

1. `MeterMaster_ESP32_Node.ino` öffnen
2. `Sketch → Hochladen`

---

## 🚀 Erster Start

1. **WLAN-Setup:** Beim ersten Start öffnet der ESP32 den Access Point `MeterMaster-Setup`. Im Browser `192.168.4.1` öffnen und WLAN-Daten eingeben.

2. **Web-Interface:** Nach der Verbindung ist das Interface unter der zugewiesenen IP erreichbar (wird kurz auf dem OLED angezeigt).

3. **ioBroker verbinden:** Tab `Einstellungen` → IP des ioBroker-Servers eintragen (Port `8087` für Simple-API) → `Zähler laden` → gewünschten State auswählen → speichern.

---

## 🖥 Web-Interface

Das Interface ist über `http://{ESP32-IP}/` erreichbar und enthält 8 Tabs:

| Tab | Inhalt |
|---|---|
| **Dashboard** | Aktueller Wert, LED-Steuerung, OLED-Zähler & Stil wählen |
| **Einstellungen** | ioBroker-Host, Discover, Zähler-Liste, Adapter-Version |
| **WLAN & System** | Signalstärke, IP, MAC, Heap, Sketch-Größe, Uptime |
| **Alarm** | Schwellwert, Bedingung, LED-Blinken bei Alarm |
| **Bluetooth** | Geplante BLE-Features (deaktiviert, Flash-Speicher) |
| **Carousel** | Zähler-Liste, Intervall, Node-Registrierung & Name |
| **OTA Update** | GitHub-Versionscheck + manueller Firmware-Upload |
| **Info** | Changelog, GitHub-Links, Lizenz |

---

## 📡 ioBroker-Integration

### Voraussetzungen
- ioBroker mit [Simple-API-Adapter](https://github.com/ioBroker/ioBroker.simple-api) (Port 8087)
- [MeterMaster ioBroker-Adapter](https://github.com/MPunktBPunkt/iobroker.metermaster) ≥ v0.4.0

### Automatische Node-Registrierung

Der ESP32 schreibt sich beim Start und alle 60 Sekunden automatisch in ioBroker ein:

```
metermaster.0.nodes.{MAC}.ip         → 192.168.178.110
metermaster.0.nodes.{MAC}.name       → MeterMaster Node
metermaster.0.nodes.{MAC}.version    → 1.5.0
metermaster.0.nodes.{MAC}.lastSeen   → 1234567890000  (Unix-Timestamp ms)
```

### Fernsteuerung durch ioBroker

Der Adapter kann den ESP32 fernsteuern, indem er den `config`-State beschreibt:

```
metermaster.0.nodes.{MAC}.config  →  {"sid":"...","label":"...","unit":"..."}
```

Der ESP32 liest diesen State alle 15 Sekunden und übernimmt die Konfiguration sofort.

**Vollständiges Config-Format:**
```json
{
  "sid": "metermaster.0.MeinHaus.Westerheim.Strom.readings.latest",
  "label": "Strom",
  "unit": "kWh",
  "carouselActive": true,
  "carouselSec": 10,
  "carousel": [
    {"sid": "metermaster.0...Strom.readings.latest",      "label": "Strom",      "unit": "kWh"},
    {"sid": "metermaster.0...Warmwasser.readings.latest", "label": "Warmwasser", "unit": "m³"}
  ]
}
```

---

## 🔌 API-Endpunkte

Alle Endpunkte erreichbar unter `http://{ESP32-IP}/`

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/version` | Firmware-Version + GitHub-Infos |
| GET | `/api/status` | Aktueller Wert, LED, Alarm, OLED-Stil |
| GET | `/api/settings` | Alle Einstellungen lesen |
| POST | `/api/settings` | Einstellungen speichern |
| POST | `/api/alarm` | Alarm konfigurieren |
| POST | `/api/oled` | OLED-Zähler + Stil setzen |
| GET | `/api/discover` | Alle `metermaster.*`-States laden |
| GET | `/api/led?state=on\|off\|toggle` | LED steuern |
| GET | `/api/sysinfo` | WLAN, Heap, Chip, Uptime |
| GET | `/api/nodeinfo` | Vollständige Node-Info (für ioBroker-Adapter) |
| GET | `/api/carousel` | Carousel lesen |
| POST | `/api/carousel` | Carousel konfigurieren |
| GET | `/api/carousel/next` | Manuell weiterblättern |
| GET | `/api/nodename?name=...` | Gerätename setzen |
| POST | `/update` | OTA Firmware-Upload |

---

## 🎠 Carousel-Modus

Der Carousel-Modus zeigt mehrere Zähler nacheinander auf dem OLED an.

**Konfigurieren:** Tab `Carousel` → Zähler hinzufügen (aus Discover-Liste oder manuell) → Reihenfolge per ▲▼ → Intervall in Sekunden → `Speichern`

- Bis zu **5 Zähler** pro Node
- Konfigurierbares **Intervall** (3–300 Sekunden)
- Manuelles Weiterblättern per Button
- Persistente Speicherung (überlebt Neustart)
- Fernsteuerung durch ioBroker-Adapter möglich

---

## 🔔 Alarm

| Einstellung | Beschreibung |
|---|---|
| Schwellwert | Numerischer Grenzwert |
| Bedingung | Alarm wenn Wert **über** oder **unter** Schwellwert |
| Aktion | Rote LED blinkt, OLED zeigt `!ALM` |

---

## 📟 OLED Display-Stile

| # | Name | Layout |
|---|---|---|
| 0 | **Standard** | Label · Wert · Einheit · Uhrzeit · Datum · Status |
| 1 | **Groß** | Label · großer Wert (textSize 2) · Einheit |
| 2 | **Minimal** | Nur Wert + Einheit, zentriert |
| 3 | **Invertiert** | Weißer Hintergrund, schwarzer Text |

---

## 🔄 OTA Update

**Manuell:** Tab `OTA Update` → `.bin`-Datei hochladen  
**.bin erzeugen:** Arduino IDE → `Sketch → Exportiere kompilierte Binärdatei`

**GitHub-Check:** Tab `OTA Update` → `Prüfen` → prüft automatisch auf neue Releases in diesem Repository → direkter Download-Link bei verfügbarer Version

---

## 📁 Projektstruktur

```
esp32.MeterMaster/
├── MeterMaster_ESP32_Node.ino   Hauptsketch (single-file)
└── README.md                    Diese Datei
```

---

## 🗺 Roadmap

- [ ] OTA direkt aus GitHub Release laden (ohne manuellen Download)
- [ ] Physischer Button (GPIO) für Carousel-Wechsel
- [ ] BLE-Scan (Partition Scheme: Huge APP)
- [ ] Deep-Sleep für Batteriebetrieb
- [ ] ioBroker-Adapter: Admin-UI für Node-Verwaltung
- [ ] ioBroker-Adapter: OTA-Trigger über config-State

---

## 🔗 Verwandte Projekte

| Projekt | Link | Beschreibung |
|---|---|---|
| MeterMaster ioBroker Adapter | [iobroker.metermaster](https://github.com/MPunktBPunkt/iobroker.metermaster) | Node.js Adapter für ioBroker |
| MeterMaster App | [MPunktBPunkt](https://github.com/MPunktBPunkt) | .NET MAUI Android App |

---

## 📄 Lizenz

MIT License – siehe [LICENSE](LICENSE)

---

<p align="center">
  Entwickelt als Companion-Hardware für das MeterMaster-Ökosystem<br>
  <a href="https://github.com/MPunktBPunkt/iobroker.metermaster">MPunktBPunkt/iobroker.metermaster</a>
</p>
