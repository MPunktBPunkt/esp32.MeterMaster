# ESP32 ↔ ioBroker – Datenaustausch

> **Status:** Implementiert und aktiv (v0.4.1 / Adapter v0.7.0, Stand 13.03.2026)  
> Dieses Dokument beschreibt die vollständige Kommunikationsarchitektur zwischen dem  
> ESP32 Display-Node und dem ioBroker MeterMaster-Adapter.

---

## Übersicht – Zwei Kommunikationskanäle

| Kanal | Richtung | Port | Verwendung |
|---|---|---|---|
| **ioBroker Simple-API** | ESP32 → ioBroker | 8087 | Nur Zählerwerte lesen + Discover |
| **MeterMaster Adapter** | ESP32 ↔ Adapter | 8089 | Registrierung, Config, Cmd |

Der ESP32 agiert immer als **HTTP-Client** – er initiiert alle Anfragen.  
States (`metermaster.0.nodes.*`) werden **nicht** über simple-api geschrieben, sondern der Adapter legt sie selbst an, wenn der ESP32 sich registriert.

> **Warum nicht simple-api für Registrierung?**  
> Die simple-api kann nur *bereits angelegte* States schreiben. Die `nodes.*`-States existieren erst nach der ersten Adapter-Registrierung – ein Henne-Ei-Problem. Daher kommuniziert der ESP32 direkt mit dem Adapter-HTTP-Server.

---

## 1 · Zählerwerte lesen (Simple-API)

Der ESP32 liest Zählerwerte direkt über die ioBroker Simple-API.

**Endpunkt:**
```
GET http://{iobHost}:8087/getPlainValue/{stateId}
```

**Beispiel:**
```
GET http://192.168.178.113:8087/getPlainValue/metermaster.0.MeinHaus.Westerheim.Warmwasser.readings.latest
→ 94.05
```

**Intervall:** konfigurierbar im Einstellungen-Tab (Standard: 60 s)  
**State-ID:** wird in NVS (`Preferences`) unter Key `sid` gespeichert

---

## 2 · Verfügbare Zähler entdecken (Discover)

Beim Klick auf „Zähler laden" im Einstellungen-Tab lädt der ESP32 die Discover-Liste direkt vom Adapter.

**Endpunkt (Adapter-API):**
```
GET http://{iobHost}:8089/api/discover
```

**Antwort:**
```json
[
  {
    "stateId":   "metermaster.0.MeinHaus.Westerheim.Warmwasser.readings.latest",
    "label":     "Warmwasser",
    "unit":      "m³",
    "typeName":  "HotWater",
    "house":     "MeinHaus",
    "apartment": "Westerheim",
    "meter":     "Warmwasser",
    "latest":    94.049
  }
]
```

---

## 3 · Node-Registrierung / Heartbeat (ESP32 → Adapter)

Beim Start und danach alle **60 Sekunden** meldet sich der ESP32 beim Adapter an.

**Endpunkt:**
```
POST http://{iobHost}:8089/api/register
Content-Type: application/json

{
  "mac":     "C8C9A3CB7B08",
  "ip":      "192.168.178.110",
  "name":    "MeterMaster Node",
  "version": "0.4.1"
}
```

**Erfolgsantwort:**
```json
{ "ok": true, "mac": "C8C9A3CB7B08", "lastSeen": 1741000000000 }
```

**Was der Adapter dabei tut:**
- Legt `metermaster.0.nodes.{MAC}.*` States beim ersten Heartbeat automatisch an
- Aktualisiert `ip`, `name`, `version`, `lastSeen` in nodesCache + ioBroker States
- Falls der Adapter neu gestartet wurde: der ESP32 re-registriert sich spätestens nach 60 s

**MAC-Format:** `C8C9A3CB7B08` (ohne Doppelpunkte, Großbuchstaben)

---

## 4 · Config-Poll (Adapter → ESP32)

Der ESP32 fragt alle **15 Sekunden** seine Konfiguration und ausstehende Befehle ab.

**Endpunkt:**
```
GET http://{iobHost}:8089/api/nodes/{MAC}/config
```

**Antwort:**
```json
{
  "ok":  true,
  "config": "{\"sid\":\"metermaster.0.MeinHaus.Westerheim.Warmwasser.readings.latest\",\"label\":\"Warmwasser\",\"unit\":\"m³\",\"carouselActive\":false,\"carouselSec\":10,\"carousel\":[]}",
  "cmd":  "{\"ledOn\": true}"
}
```

| Feld | Typ | Bedeutung |
|---|---|---|
| `ok` | bool | Anfrage erfolgreich |
| `config` | string\|null | JSON-String mit Zähler-Konfiguration, null = keine Änderung |
| `cmd` | string\|null | JSON-String mit Sofortbefehl, null = kein Befehl; wird nach Auslieferung vom Adapter gelöscht |

### Config-Felder

| Feld | Typ | Beschreibung |
|---|---|---|
| `sid` | string | State-ID des anzuzeigenden Zählers |
| `label` | string | Anzeigetext auf dem OLED |
| `unit` | string | Einheit (m³, kWh, ...) |
| `carouselActive` | bool | Carousel ein/aus |
| `carouselSec` | number | Wechselintervall in Sekunden |
| `carousel` | array | Bis zu 5 Einträge mit `sid`, `label`, `unit` |

Felder die nicht vorhanden sind werden ignoriert – der ESP32 behält seinen lokalen Wert.

---

## 5 · ConfigAck (ESP32 → Adapter)

Nach Übernahme einer neuen Config quittiert der ESP32:

**Endpunkt:**
```
POST http://{iobHost}:8089/api/nodes/{MAC}/configAck
Content-Type: application/json

{ "ack": "0.4.1@12345" }
```

Der Adapter speichert den ack-Wert im State `nodes.{MAC}.configAck`.

---

## 6 · Sofortbefehle – cmd (Adapter → ESP32)

Der Adapter kann Sofortbefehle setzen, die beim nächsten Config-Poll (max. 15 s) ausgeführt werden. Der Befehl wird danach vom Adapter automatisch gelöscht (einmalige Auslieferung).

**Befehle über Adapter-API:**
```
POST http://{iobHost}:8089/api/nodes/{MAC}/cmd
Content-Type: application/json
Authorization: Basic ...

{ "ledOn": true }
```

| Befehl | Payload | Wirkung am ESP32 |
|---|---|---|
| LED ein | `{"ledOn": true}` | LED einschalten |
| LED aus | `{"ledOn": false}` | LED ausschalten |
| Zähler wechseln | `{"sid":"...", "label":"...", "unit":"..."}` | Sofortiger Zählerwechsel + Speichern |

---

## 7 · `/api/nodeinfo` – direkter Adapter-Zugriff auf ESP32

Der Adapter kann optional online Nodes direkt befragen (z.B. für Live-Status in der Web-UI).

**Endpunkt (am ESP32):**
```
GET http://{nodeIp}/api/nodeinfo
```

**Antwort:**
```json
{
  "mac":            "C8C9A3CB7B08",
  "ip":             "192.168.178.110",
  "name":           "MeterMaster Node",
  "version":        "0.4.1",
  "uptime":         3600,
  "rssi":           -68,
  "stateId":        "metermaster.0.MeinHaus.Westerheim.Warmwasser.readings.latest",
  "label":          "Warmwasser",
  "unit":           "m³",
  "value":          94.049,
  "fetchOk":        true,
  "carouselActive": false,
  "carouselIdx":    0,
  "carouselSec":    10,
  "carousel":       []
}
```

---

## 8 · ioBroker State-Struktur

```
metermaster.0.
└── nodes/
    └── C8C9A3CB7B08/          ← MAC-Adresse (ohne Doppelpunkte)
        ├── ip          string  "192.168.178.110"   read-only (Adapter schreibt)
        ├── name        string  "MeterMaster Node"  read-only (Adapter schreibt)
        ├── version     string  "0.4.1"             read-only (Adapter schreibt)
        ├── lastSeen    number  1741000000000       read-only (Adapter schreibt, ms)
        ├── config      string  "{...}"             writable  (Adapter schreibt, ESP32 liest)
        ├── configAck   string  "0.4.1@12345"       read-only (Adapter schreibt nach Ack)
        └── cmd         string  "{...}"             writable  (Adapter schreibt, ESP32 liest+löscht)
```

Alle States werden vom Adapter bei der ersten Registrierung automatisch angelegt.

---

## 9 · Timing-Übersicht

| Aktion | Intervall | Auslöser |
|---|---|---|
| Zählerwert lesen (Simple-API) | konfigurierbar (Standard 60 s) | Timer in `loop()` |
| Node-Heartbeat (Adapter) | 60 s + beim Start | Timer in `loop()` |
| Config+cmd-Poll (Adapter) | 15 s | Timer in `loop()` |
| Discover (Adapter) | on-demand | Button im Einstellungen-Tab |
| Carousel weiterschalten | konfigurierbar (Standard 10 s) | Timer in `loop()` |

---

## 10 · Voraussetzungen

- **ioBroker Simple-API Adapter** installiert und aktiv (Port 8087)
- **iobroker.metermaster Adapter** >= v0.7.0 installiert und aktiv (Port 8089)
- Zähler-Daten wurden bereits über die MeterMaster App in ioBroker synchronisiert
