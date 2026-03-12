# ESP32 ↔ ioBroker Adapter – Datenaustausch

> **Status:** Implementiert und im Feldeinsatz bestätigt (v0.3.0, 12.03.2026)  
> Adapter-Gegenseite (iobroker.metermaster >= v0.5.0) steht noch aus.
>
> Bestätigt funktionsfähig: Node-Registrierung, Heartbeat, Simple-API Fetch, Discover
>
> ~~Beschreibt den geplanten Datenaustausch~~ für `iobroker.metermaster` >= v0.5.0  
> Der Adapter wird in einem separaten Schritt angepasst.  
> ESP32-Firmware: `MeterMaster_ESP32_Node` ab v0.3.0

---

## Übersicht

Der ESP32-Node kommuniziert mit dem ioBroker-Adapter über **zwei Kanäle**:

| Kanal | Richtung | Protokoll | Verwendung |
|---|---|---|---|
| ioBroker Simple-API | ESP32 → ioBroker | HTTP GET/SET | Zählerwerte lesen, Node registrieren |
| ioBroker Simple-API | ioBroker → ESP32 (indirekt) | HTTP GET (poll) | Config vom Adapter übernehmen |

Der ESP32 agiert immer als **HTTP-Client** – er initiiert alle Anfragen.  
Der Adapter schreibt Steuerbefehle als ioBroker-States, die der ESP32 per Poll abholt.

---

## 1 · Zählerwerte lesen

Der ESP32 liest Zählerwerte direkt über die ioBroker Simple-API.

**Endpunkt:**
```
GET http://{iobHost}:{iobPort}/getPlainValue/{stateId}
```

**Beispiel:**
```
GET http://192.168.178.113:8087/getPlainValue/metermaster.0.MeinHaus.Westerheim.Warmwasser.readings.latest
→ 94.05
```

**Intervall:** konfigurierbar im Einstellungen-Tab (Standard: 60 s)  
**Konfigurierter State:** wird in NVS (`Preferences`) unter Key `sid` gespeichert

---

## 2 · Verfügbare Zähler entdecken (Discover)

Beim Klick auf „Zähler laden" im Einstellungen-Tab ruft der ESP32 alle `metermaster.*`-States ab.

**Endpunkt:**
```
GET http://{iobHost}:{iobPort}/getStates/metermaster.*
```

**Antwort (Simple-API):**
```json
{
  "metermaster.0.MeinHaus.Westerheim.Warmwasser.readings.latest": {
    "val": 94.05,
    "ack": true,
    "ts": 1709123456789
  },
  ...
}
```

Der ESP32 filtert States heraus, deren `val` numerisch ist (float/int), und leitet daraus Label und Einheit ab.

---

## 3 · Node-Registrierung (ESP32 → ioBroker)

Beim Start und alle **60 Sekunden** schreibt der ESP32 seine Präsenz in ioBroker.

**Endpunkt (Simple-API SET):**
```
GET http://{iobHost}:{iobPort}/set/{stateId}?value={wert}
```

**Geschriebene States** (Namespace: `metermaster.0.nodes.{MAC}`):

| State | Typ | Beispielwert | Beschreibung |
|---|---|---|---|
| `nodes.{MAC}.ip` | string | `192.168.178.110` | Aktuelle IP-Adresse |
| `nodes.{MAC}.name` | string | `MeterMaster Node` | Anzeigename (editierbar im Carousel-Tab) |
| `nodes.{MAC}.version` | string | `0.2.0` | Firmware-Version |
| `nodes.{MAC}.lastSeen` | number | `1709123456789` | Unix-Timestamp (ms) |

**MAC-Format:** `C8C9A3CB7B08` (ohne Doppelpunkte, Großbuchstaben)

**Adapter-Aufgabe (TODO):**  
Diese States müssen im Adapter als `type: "state"` mit passendem `role` und `type` angelegt werden, wenn sie noch nicht existieren.

---

## 4 · Config-Poll (ioBroker → ESP32)

Der ESP32 liest alle **15 Sekunden** einen Config-State, den der Adapter beschreiben kann.

**Endpunkt:**
```
GET http://{iobHost}:{iobPort}/getPlainValue/metermaster.0.nodes.{MAC}.config
```

**Erwarteter Wert:** JSON-String (serialisiert als ioBroker string-State)

```json
{
  "sid":             "metermaster.0.MeinHaus.Westerheim.Warmwasser.readings.latest",
  "label":           "Warmwasser",
  "unit":            "m³",
  "carouselActive":  false,
  "carouselSec":     10,
  "carousel": [
    { "sid": "...readings.latest", "label": "Warmwasser", "unit": "m³" },
    { "sid": "...readings.latest", "label": "Kaltwasser",  "unit": "m³" }
  ]
}
```

| Feld | Typ | Pflicht | Beschreibung |
|---|---|---|---|
| `sid` | string | nein | State-ID für Einzelanzeige (ersetzt aktuelle Einstellung) |
| `label` | string | nein | Anzeigetext auf OLED |
| `unit` | string | nein | Einheit (m³, kWh, ...) |
| `carouselActive` | bool | nein | Carousel ein/aus |
| `carouselSec` | number | nein | Wechselintervall in Sekunden |
| `carousel` | array | nein | Carousel-Einträge (max. 5) |

Felder die nicht vorhanden sind werden ignoriert – der ESP32 behält seinen lokalen Wert.

**Nach Übernahme:** ESP32 schreibt Bestätigung:
```
GET http://{iobHost}:{iobPort}/set/metermaster.0.nodes.{MAC}.configAck?value=1
```

**Adapter-Aufgabe (TODO):**  
- State `nodes.{MAC}.config` als beschreibbaren string-State anlegen
- State `nodes.{MAC}.configAck` als number-State anlegen (read-only aus Adapter-Sicht)
- Admin-UI: Config per JSON-Editor oder Formular befüllen und in den State schreiben

---

## 5 · `/api/nodeinfo` – Adapter-Abfrage am ESP32

Der Adapter kann optional aktive Nodes direkt befragen.

**Endpunkt (am ESP32):**
```
GET http://{nodeIp}/api/nodeinfo
```

**Antwort:**
```json
{
  "mac":           "C8C9A3CB7B08",
  "ip":            "192.168.178.110",
  "name":          "MeterMaster Node",
  "version":       "0.2.0",
  "sid":           "metermaster.0.MeinHaus.Westerheim.Warmwasser.readings.latest",
  "label":         "Warmwasser",
  "unit":          "m³",
  "value":         94.05,
  "fetchOk":       true,
  "carouselActive":false,
  "carouselCount": 0,
  "uptime":        387
}
```

Damit kann der Adapter eine Node-Liste im Admin-UI anzeigen, ohne auf die ioBroker-States angewiesen zu sein.

---

## 6 · State-Struktur im ioBroker (Soll)

```
metermaster.0
└── nodes
    └── C8C9A3CB7B08          ← MAC-Adresse (ohne Doppelpunkte)
        ├── ip          string  "192.168.178.110"
        ├── name        string  "MeterMaster Node"
        ├── version     string  "0.2.0"
        ├── lastSeen    number  1709123456789
        ├── config      string  "{...}"   ← Adapter schreibt, ESP32 liest
        └── configAck   number  1         ← ESP32 schreibt, Adapter liest
```

---

## 7 · Timing-Übersicht

| Aktion | Intervall | Auslöser |
|---|---|---|
| Zählerwert lesen | konfigurierbar (Standard 60 s) | Timer in `loop()` |
| Node registrieren | 60 s + beim Start | Timer in `loop()` |
| Config-Poll | 15 s | Timer in `loop()` |
| Discover | on-demand | Button im Web-UI |

---

## 8 · Voraussetzungen am ioBroker

- **Simple-API Adapter** muss installiert und aktiv sein
- Standard-Port: `8087` (im ESP32 Einstellungen-Tab konfigurierbar)
- Simple-API muss `getStates`, `getPlainValue` und `set` unterstützen (alle Standard-Features)
- `metermaster.metermaster` Adapter muss laufen (liefert die Zähler-States)

---

## 9 · Adapter-TODOs (iobroker.metermaster >= v0.5.0)

```
[ ] nodes.{MAC}.* States automatisch anlegen wenn Node sich registriert
[ ] nodes.{MAC}.config State als beschreibbar definieren (type: string)
[ ] nodes.{MAC}.configAck auswerten (Bestätigung dass Config übernommen wurde)
[ ] Admin-UI Tab "Nodes": Liste aller registrierten Nodes anzeigen
[ ] Admin-UI: Config-State per Formular befüllen (Zählerauswahl + Carousel)
[ ] Heartbeat-Überwachung: Alarm wenn lastSeen > 5 Minuten
[ ] Optional: /api/nodeinfo direkt am ESP32 abfragen für Live-Status
```
