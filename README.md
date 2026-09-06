# esp32.MeterMaster

![Version](https://img.shields.io/badge/version-0.4.3-blue)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Donate](https://img.shields.io/badge/Donate-PayPal-00457C.svg?logo=paypal)](https://www.paypal.com/donate/?business=martin%40bchmnn.de&currency_code=EUR)

> **OLED-Display-Node für MeterMaster** — Zählerwerte aus ioBroker auf einem 64×48-Display. Steuerbar über Web-UI, [iobroker.metermaster](https://github.com/MPunktBPunkt/iobroker.metermaster) und optional sichtbar im [ESP-Hub](https://github.com/MPunktBPunkt/iobroker.esp-hub).

---

## Überblick

Der MeterMaster ESP32 Node holt Zählerstände aus ioBroker und zeigt sie auf einem kleinen OLED. Er registriert sich beim **MeterMaster-Adapter** (Port 8089) für Config, Fernsteuerung und Carousel — und sendet zusätzlich einen Heartbeat an den **ESP-Hub** (Port 8093), damit das Gerät im zentralen ESP-Dashboard erscheint.

---

## Features

- **Zählerwerte live** — via ioBroker Simple-API (Port 8087)
- **4 OLED-Stile** — Standard, Groß, Minimal, Invertiert
- **Carousel** — bis zu 5 Zähler automatisch wechseln
- **Alarm** — LED-Blinken + OLED-Warnung bei Schwellwert
- **MeterMaster-Adapter** — Register, Config-Poll, cmd (LED, Zähler wechseln)
- **ESP-Hub** — zusätzliche Sichtbarkeit im ESP-Dashboard (ab v0.4.2)
- **OTA** — Browser-Upload oder GitHub-Release
- **WiFiManager** — Hotspot `MeterMaster-Setup`

---

## Hardware

| Komponente | Details |
|------------|---------|
| Mikrocontroller | ESP32 D1 Mini (WEMOS) |
| Display | 0.66" OLED 64×48, SSD1306, I²C |
| SDA / SCL | GPIO 21 / 22 |
| LED | GPIO 2 (active HIGH) |

---

## Voraussetzungen

| Library | Autor | Version |
|---------|-------|---------|
| WiFiManager | tzapu | ≥ 2.0.17 |
| U8g2 | olikraus | ≥ 2.34 |
| ArduinoJson | Blanchon | ≥ 6.21 |

**Board:** WEMOS D1 MINI ESP32

---

## Quickstart

1. `MeterMaster_ESP32_Node.ino` flashen
2. Hotspot **`MeterMaster-Setup`** → WLAN konfigurieren
3. Web-UI: `http://<ESP-IP>/`
4. **Einstellungen:** ioBroker-Host, Simple-API (8087), Adapter (8089), ESP-Hub (8093)
5. Zähler laden oder State-ID manuell eingeben

---

## Zwei ioBroker-Anbindungen

| Adapter | Port | Aufgabe |
|---------|------|---------|
| **iobroker.metermaster** | 8089 | Node-Register, Config, cmd, Fernsteuerung |
| **iobroker.esp-hub** | 8093 | Dashboard-Sichtbarkeit, IO-Werte (optional, 0 = aus) |

### MeterMaster-Adapter (Pflicht für volle Funktion)

```
POST :8089/api/register          ← Heartbeat alle 60 s
GET  :8089/api/nodes/{MAC}/config ← Config-Poll alle 15 s
```

States: `metermaster.0.nodes.<MAC>.*`

### ESP-Hub (optional, empfohlen)

```
POST :8093/api/register          ← Heartbeat alle 60 s
```

States: `esp-hub.0.devices.<MAC>.*` mit IO-Werten `value`, `carousel`, `adapter`

---

## Web-Oberfläche

| Tab | Inhalt |
|-----|--------|
| **Dashboard** | Zählerwert, Adapter- & Hub-Status |
| **Einstellungen** | ioBroker, Adapter, ESP-Hub, Zählerauswahl |
| **Carousel** | Mehrere Zähler rotieren |
| **Alarm** | Schwellwert + LED |
| **OTA** | Firmware-Update |
| **Debug** | Live-Log, Neustart |

---

## API (Auszug)

| Endpunkt | Beschreibung |
|----------|--------------|
| `/api/status` | Zählerwert, Adapter- & Hub-Status |
| `/api/settings` | GET/POST Einstellungen |
| `/api/testadapter` | MeterMaster-Adapter testen |
| `/api/testhub` | ESP-Hub testen |
| `/api/carousel` | Carousel konfigurieren |

---

## Lizenz

MIT © MPunktBPunkt — siehe [LICENSE](LICENSE)

Companion für [MeterMaster App](https://github.com/MPunktBPunkt/MeterMaster) und [iobroker.metermaster](https://github.com/MPunktBPunkt/iobroker.metermaster).

[![Donate](https://img.shields.io/badge/Donate-PayPal-00457C.svg?logo=paypal)](https://www.paypal.com/donate/?business=martin%40bchmnn.de&currency_code=EUR)
