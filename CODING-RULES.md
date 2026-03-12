# CODING-RULES – MeterMaster ESP32 Node
> Fokus: Lesbarkeit · Dokumentation · Wartbarkeit  
> Gilt für: `MeterMaster_ESP32_Node.ino` und alle zukünftigen `.ino`/`.h` Dateien

---

## 1 · Dateistruktur

Jede `.ino`-Datei folgt dieser festen Reihenfolge:

```
1.  Datei-Header (Name, Version, Kurzbeschreibung)
2.  #include
3.  #define  –  Konstanten & Hardware-Pins
4.  Globale Variablen  –  gruppiert mit Kommentar-Banner
5.  Hilfsfunktionen  (NTP, OLED, Alarm, ...)
6.  PROGMEM HTML/JS  (H_HEAD, H_DB, ..., H_JS)
7.  Web-Handler  (hRoot, hApiStatus, ...)
8.  setup()
9.  loop()
```

---

## 2 · Datei-Header

Jede Datei beginnt mit einem standardisierten Block:

```cpp
/*
 * ============================================================
 *  MeterMaster ESP32 Node  –  v1.5.0
 *  Hardware: ESP32 D1 Mini + 64×48 OLED (SSD1306, I²C)
 * ============================================================
 *
 *  Kurzbeschreibung:
 *    WLAN-Display-Node für das MeterMaster-Ökosystem.
 *    Zeigt ioBroker-Zählerwerte auf OLED, registriert sich
 *    im Adapter und unterstützt Carousel-Anzeige.
 *
 *  Autor:    MPunktBPunkt
 *  Lizenz:   MIT
 *
 *  Libraries:
 *    - WiFiManager       by tzapu      >= 2.0.17
 *    - Adafruit SSD1306  by Adafruit   >= 2.5
 *    - ArduinoJson       by Blanchon   >= 6.21
 *
 *  Changelog:
 *    v1.5.0 – Carousel, Node-Registrierung, Config-Poll
 *    v1.4.0 – OTA GitHub-Check, 4 OLED-Stile, Info-Tab
 * ============================================================
 */
```

---

## 3 · Funktions-Dokumentation

Jede Funktion erhält einen Dokumentationskommentar **direkt darüber**.  
Format: Einzeiler für einfache Funktionen, Doxygen-Block für komplexe.

### Einfache Funktion
```cpp
// Setzt die LED auf den gewünschten Zustand (active HIGH).
void setLed(bool on) {
  digitalWrite(LED_PIN, on ? HIGH : LOW);
  ledOn = on;
}
```

### Komplexe Funktion (Doxygen-Block)
```cpp
/**
 * Holt den aktuellen Wert eines ioBroker-States via Simple-API.
 *
 * @param stateId  Vollständiger State-Pfad, z.B. "metermaster.0.X.readings.latest"
 * @param ok       Wird auf true gesetzt wenn der Abruf erfolgreich war
 * @param errMsg   Enthält die Fehlermeldung bei Misserfolg, sonst ""
 * @return         Numerischer Wert des States, 0.0 bei Fehler
 */
float fetchStateValue(const String& stateId, bool& ok, String& errMsg) {
  ...
}
```

### Wann welches Format?

| Funktion | Format |
|---|---|
| Setter / Getter, ≤ 5 Zeilen | Einzeiler `//` |
| Hilfsfunktion ohne Parameter | Einzeiler `//` |
| API-Handler (`hApi*`) | Einzeiler `// METHODE /pfad – Beschreibung` |
| Funktion mit ≥ 2 Parametern | Doxygen `/** */` |
| Funktion mit Rückgabewert + Seiteneffekten | Doxygen `/** */` |

---

## 4 · Kommentar-Banner für Abschnitte

Zusammengehörige Funktionen werden mit einem Banner gruppiert:

```cpp
// ─────────────────────────────────────────────────────────────────────────────
//  Carousel
// ─────────────────────────────────────────────────────────────────────────────
void saveCarousel() { ... }
void loadCarousel() { ... }
void carouselNext() { ... }
```

Kurzform für kleinere Gruppen:
```cpp
// ── LED ───────────────────────────────────────────────────────────────────────
void setLed(bool on)  { ... }
void handleBlink()    { ... }
```

---

## 5 · `#define` Konstanten

Alle `#define`-Konstanten werden kommentiert und in Gruppen gegliedert:

```cpp
// ── Hardware ──────────────────────────────────────────────────────────────────
#define SDA_PIN    21           // I²C Data
#define SCL_PIN    22           // I²C Clock
#define LED_PIN    2            // Rote LED, active HIGH
#define OLED_ADDR  0x3C         // SSD1306 Standard-Adresse

// ── Firmware ──────────────────────────────────────────────────────────────────
#define FW_VERSION  "1.5.0"     // Semantische Versionierung: MAJOR.MINOR.PATCH
#define GH_USER     "MPunktBPunkt"
#define GH_REPO_ESP "esp32.MeterMaster"

// ── Timing ────────────────────────────────────────────────────────────────────
#define REGISTER_INTERVAL   60000   // ms – ioBroker Heartbeat-Intervall
#define CONFIGPOLL_INTERVAL 15000   // ms – Config-Poll vom Adapter
```

Keine Magic Numbers direkt im Code:
```cpp
// ❌
if (millis() - lastRegister >= 60000) { ... }

// ✅
if (millis() - lastRegister >= REGISTER_INTERVAL) { ... }
```

---

## 6 · Globale Variablen

Variablen werden in **benannte Gruppen** mit Kommentar-Banner eingeteilt.  
Inline-Kommentar wenn der Name nicht vollständig selbsterklärend ist:

```cpp
// ── Verbindung & Einstellungen ────────────────────────────────────────────────
String iobHost   = "192.168.178.113";  // IP des ioBroker-Servers
int    iobPort   = 8087;               // Simple-API Port
int    fetchSec  = 60;                 // Fetch-Intervall in Sekunden

// ── Laufzeit ──────────────────────────────────────────────────────────────────
float         currentValue  = 0.0;    // Zuletzt gelesener Zählerwert
bool          fetchOk       = false;  // true wenn letzter HTTP-Fetch OK
String        statusMsg     = "Init"; // Statustext für Web-Interface
unsigned long lastFetch     = 0;      // millis() des letzten Fetches
```

Kein Kommentar nötig bei vollständig selbsterklärenden Namen:
```cpp
bool ntpSynced   = false;
bool alarmActive = false;
bool ledOn       = false;
```

---

## 7 · Structs und Enums

### Struct – immer vollständig dokumentiert
```cpp
/**
 * Ein Eintrag in der Carousel-Anzeige.
 * Wird persistent in NVS gespeichert (Namespace "mm-car").
 * Felder val und ok sind reine Laufzeitwerte (nicht persistent).
 */
struct CarouselEntry {
  String sid;    // ioBroker State-ID (vollständiger Pfad)
  String label;  // Anzeigetext auf dem OLED
  String unit;   // Einheit (m³, kWh, ...)
  float  val;    // Zuletzt gelesener Wert
  bool   ok;     // true wenn letzter Fetch erfolgreich war
};
```

### Enum – jeder Wert kommentiert
```cpp
// OLED-Anzeigestil, wird in Preferences unter Key "oStyl" gespeichert.
enum OledStyle {
  STYLE_STANDARD = 0,  // Label + Wert + Einheit + Zeit + Datum
  STYLE_LARGE    = 1,  // Großer Wert (textSize 2 bei <= 4 Zeichen)
  STYLE_MINIMAL  = 2,  // Nur Wert + Einheit, zentriert
  STYLE_INVERTED = 3   // Weißer Hintergrund, schwarzer Text
};
```

---

## 8 · Einrückung & Formatierung

```cpp
// Einrückung: 2 Spaces, kein Tab
void doSomething() {
  if (condition) {
    for (int i = 0; i < count; i++) {
      doWork(i);
    }
  }
}

// Klammern: immer, auch bei einzeiligem Body
// ❌
if (ok) doThing();

// ✅ kurz:
if (ok) { doThing(); }

// ✅ mehrzeilig:
if (ok) {
  doThing();
}

// Leerzeichen um Operatoren
int x = a + b;    // ✅
int x = a+b;      // ❌

// Zeilenlänge: max 100 Zeichen – lange Calls umbrechen
server.send(200, "application/json",
  String("{") + "\"ok\":true,\"mac\":\"" + nodeMac + "\"}");
```

---

## 9 · Web-Handler Konventionen

Jeder Handler hat einen Kommentar mit HTTP-Methode und Pfad.  
Interne Struktur folgt immer der Reihenfolge: **Prüfen → Parsen → Verarbeiten → Antworten**.

```cpp
// POST /api/carousel – Übernimmt neue Carousel-Konfiguration aus JSON-Body.
void hApiCarouselPost() {
  // 1. Eingabe prüfen
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", String("{\"ok\":false,\"msg\":\"Kein Body\"}"));
    return;
  }

  // 2. JSON parsen
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", String("{\"ok\":false,\"msg\":\"JSON-Fehler\"}"));
    return;
  }

  // 3. Werte übernehmen & speichern
  if (!doc["active"].isNull()) carouselActive = doc["active"].as<bool>();
  saveCarousel();

  // 4. Erfolg bestätigen
  server.send(200, "application/json", String("{\"ok\":true}"));
}
```

---

## 10 · Kommentar-Sprache & Naming

| Bereich | Konvention |
|---|---|
| Alle Kommentare | Deutsch |
| Variablen- und Funktionsnamen | Englisch, camelCase |
| Handler-Funktionen | Präfix `h` + PascalCase → `hApiStatus()` |
| Konstanten (`#define`) | UPPER\_SNAKE\_CASE → `FW_VERSION` |
| Structs | PascalCase → `CarouselEntry` |
| Enums | PascalCase + Präfix → `STYLE_STANDARD` |
| PROGMEM-Strings | Präfix `H_` + UPPER → `H_JS`, `H_CAR` |
| NVS-Keys | kurz, max. 15 Zeichen → `"mm"`, `"oStyl"` |

---

## 11 · Checkliste vor jedem Commit

```
Datei-Header
[ ] Versionsnummer aktualisiert?
[ ] Changelog-Eintrag für diese Version vorhanden?

Dokumentation
[ ] Jede neue Funktion hat einen Kommentar (Einzeiler oder Doxygen)?
[ ] Jedes neue Struct hat /** */ mit Feldbeschreibungen?
[ ] Jedes neue Enum hat Kommentare an jedem Wert?
[ ] Neue #define-Konstanten sind kommentiert?
[ ] Neue Variablengruppen haben ein Banner?

Formatierung
[ ] Einrückung: 2 Spaces, kein Tab?
[ ] Alle if/for/while haben geschweifte Klammern?
[ ] Leerzeichen um Operatoren?
[ ] Zeilenlänge <= 100 Zeichen?

Wartbarkeit
[ ] Keine Magic Numbers direkt im Code?
[ ] Kein duplizierter Code – gemeinsame Logik in Hilfsfunktion ausgelagert?
[ ] Handler folgen Prüfen → Parsen → Verarbeiten → Antworten?
```
