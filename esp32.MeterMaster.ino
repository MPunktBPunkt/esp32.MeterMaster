/*
 * ============================================================
 *  MeterMaster ESP32 Display Node  –  v0.5.0
 *  Hardware: ESP32 D1 Mini + 64×48 OLED (SSD1306, I²C)
 * ============================================================
 *
 *  Autor:   MPunktBPunkt  –  https://github.com/MPunktBPunkt
 *  Lizenz:  MIT
 *
 *  Libraries (Arduino Library Manager):
 *    - WiFiManager          by tzapu       >= 2.0.17
 *    - U8g2                 by olikraus    >= 2.34
 *    - ArduinoJson          by Blanchon    >= 6.21
 *
 *  Changelog:
 *    v0.5.0  – esp-hub-base Shared-Core A/B: HardwareInfo/CpuLoad, gzip Web-UI
 *    v0.4.3  – Zähler laden via Adapter /api/discover; ConfigAck nur bei echter Änderung;
 *              Heartbeat-Register nur noch Serial (kein Log-Spam)
 *    v0.4.1  – cmd-Verarbeitung: LED & Zähler per Adapter fernsteuern
 *              Heartbeat-Kachel im Dashboard; Adapter-Verbindungstest
 *              Bugfix: oledStyle wird jetzt persistent gespeichert (NVS "oStyl")
 *    v0.4.0  – Registrierung direkt beim MeterMaster-Adapter (Port 8089)
 *              statt simple-api; adapterPort-Einstellung; registerOk-Status
 *    v0.3.0  – U8g2 OLED-Library; Carousel (bis zu 5 Zähler)
 *              OTA GitHub-Dropdown mit semver-Vergleich; Debug-Tab
 *    v0.1.4  – Info-Tab, GitHub OTA-Check, 4 OLED-Stile
 *    v0.1.3  – Alarm-Tab, Bluetooth-Platzhalter
 *    v0.1.2  – NTP, WLAN-Tab, LED-Steuerung, Lila Theme
 *    v0.1.1  – Discover-Dropdown, OTA, Web-Interface
 *    v0.1.0  – Erstveröffentlichung
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <time.h>
#include "esphub/HardwareInfo.h"
#include "esphub/CpuLoad.h"
#include "web/UiPages.h"
// ── Hardware ──────────────────────────────────────────────────────────────────
#define FW_VERSION  "0.5.0"
#define GH_USER     "MPunktBPunkt"
#define GH_REPO_ESP "esp32.MeterMaster"
#define GH_REPO_IOB "iobroker.metermaster"

#define SDA_PIN    21
#define SCL_PIN    22
#define SCREEN_W   64
#define SCREEN_H   48
#define OLED_ADDR  0x3C
#define LED_PIN    2         // active HIGH auf WEMOS D1 Mini ESP32

// ── NTP ───────────────────────────────────────────────────────────────────────
#define NTP_SERVER  "pool.ntp.org"
#define TZ_OFFSET   3600     // UTC+1 Winter
#define DST_OFFSET  3600     // Sommerzeit +1h (automatisch)

// U8g2: spezifischer Konstruktor für EastRising 0.66" 64×48 OLED (SSD1306)
U8G2_SSD1306_64X48_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
WebServer server(80);
Preferences prefs;
HardwareInfo hwInfo;

// ── Persistente Einstellungen ─────────────────────────────────────────────────
String iobHost      = "192.168.178.113";
int    iobPort      = 8087;   // ioBroker simple-api (Zählerwerte lesen)
int    adapterPort  = 8089;   // MeterMaster Adapter HTTP (Register, Config)
int    hubPort      = 8093;   // ESP-Hub Adapter (Dashboard), 0 = aus
String stateId      = "metermaster.0.MeinHaus.Westerheim.Warmwasser.readings.latest";
String meterLabel   = "Warmwasser";
String meterUnit    = "m³";
int    fetchSec     = 60;

// Alarm
bool   alarmEnabled   = false;
float  alarmThreshold = 100.0;
bool   alarmAbove     = true;   // true=Alarm wenn Wert > Schwellwert

// ── Laufzeit ──────────────────────────────────────────────────────────────────
bool          ledOn          = false;
bool          ledBlinking    = false;
unsigned long lastBlink      = 0;
float         currentValue   = 0.0;
bool          fetchOk        = false;
String        statusMsg      = "Init";
unsigned long lastFetch      = 0;
String        lastReadingTime= "--:--";
String        lastReadingDate= "--.--.";
bool          ntpSynced      = false;
bool          alarmActive    = false;
int           oledStyle      = 0;   // 0=Standard 1=Gross 2=Minimal 3=Invertiert

// ── Carousel ──────────────────────────────────────────────────────────────────
#define CAROUSEL_MAX 5
struct CarouselEntry { String sid; String label; String unit; float val; bool ok; };
CarouselEntry carousel[CAROUSEL_MAX];
int  carouselCount   = 0;
int  carouselIdx     = 0;
int  carouselSec     = 10;   // Sekunden pro Zähler
unsigned long lastCarousel = 0;
bool carouselActive  = false;

// ── ioBroker Node-Registration ────────────────────────────────────────────────
String nodeMac          = "";   // gesetzt nach WiFi-Verbindung
String nodeName         = "MeterMaster Node";
unsigned long lastRegister   = 0;
unsigned long lastRegisterOk = 0;  // millis() letzter erfolgreicher Heartbeat
bool          registerOk     = false;
bool          hubOk          = false;
unsigned long lastConfigPoll = 0;
unsigned long lastHubHeartbeat = 0;
unsigned long lastHubOk        = 0;
#define REGISTER_INTERVAL  60000   // alle 60s Heartbeat
#define CONFIGPOLL_INTERVAL 15000  // alle 15s Config-Poll

// ─────────────────────────────────────────────────────────────────────────────
//  Zeit
// ─────────────────────────────────────────────────────────────────────────────
void syncNTP() {
  configTime(TZ_OFFSET, DST_OFFSET, NTP_SERVER);
  struct tm ti; int t = 0;
  while (!getLocalTime(&ti) && t++ < 10) delay(500);
  ntpSynced = (t < 10);
}
String nowTimeStr() {
  struct tm ti; if (!getLocalTime(&ti)) return "--:--";
  char b[6]; strftime(b, 6, "%H:%M", &ti); return String(b);
}
String nowDateStr() {
  struct tm ti; if (!getLocalTime(&ti)) return "--.--.";
  char b[8]; strftime(b, 8, "%d.%m.", &ti); return String(b);
}
void formatTs(long long tsMs) {
  time_t t = (time_t)(tsMs / 1000);
  struct tm* ti = localtime(&t);
  char b[8];
  strftime(b, 6, "%H:%M", ti); lastReadingTime = String(b);
  strftime(b, 8, "%d.%m.", ti); lastReadingDate = String(b);
}

// ─────────────────────────────────────────────────────────────────────────────
//  OLED  (64×48 – textSize 1 = 6×8px, 10 Zeichen × 6 Zeilen)
//
//  Layout (U8g2, Baseline-Koordinaten):
//    y= 6   Zeile 1: Label         (u8g2_font_5x7_tf)
//    y= 8   Trennlinie
//    y=19   Zeile 2: Wert          (u8g2_font_7x13_tf oder 10x20)
//    y=29   Zeile 3: Einheit       (u8g2_font_5x7_tf)
//    y=37   Zeile 4: Uhrzeit       (u8g2_font_baby_tf)
//    y=45   Zeile 5: Datum         (u8g2_font_baby_tf)  + Dot (61,42)
// ─────────────────────────────────────────────────────────────────────────────

// Hilfsfunktion: String zentriert auf dem 64px breiten Display ausgeben.
void u8drawCenter(const char* s, uint8_t y) {
  uint8_t w = u8g2.getStrWidth(s);
  uint8_t x = (w < 64) ? (64 - w) / 2 : 0;
  u8g2.drawStr(x, y, s);
}

// Statusindikator: ausgefüllter Kreis = OK, Kreis = Fehler.
void u8dot(bool ok) {
  if (ok) u8g2.drawBox(60, 43, 4, 4);
  else    u8g2.drawFrame(60, 43, 4, 4);
}


// Kürzt einen UTF-8 String auf maxLen sichtbare Zeichen (nicht Bytes).
// Verhindert abgeschnittene Umlaute (ä/ö/ü = 2 Bytes in UTF-8).
String utf8Clip(const String& s, uint8_t maxLen) {
  uint8_t chars = 0;
  uint16_t i = 0;
  while (i < s.length() && chars < maxLen) {
    uint8_t b = (uint8_t)s[i];
    if      (b < 0x80) i += 1;  // ASCII
    else if (b < 0xE0) i += 2;  // 2-Byte (ä ö ü etc.)
    else if (b < 0xF0) i += 3;  // 3-Byte
    else               i += 4;  // 4-Byte
    chars++;
  }
  return s.substring(0, i);
}

// Zeigt einfache 3-Zeilen-Meldung (Startmeldungen, OTA-Status, WLAN-Setup).
void oledClear(const char* l1="", const char* l2="", const char* l3="") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0,  8, l1);
  u8g2.drawStr(0, 22, l2);
  u8g2.drawStr(0, 36, l3);
  u8g2.sendBuffer();
}

void oledValue() {
  // Wert-String aufbereiten
  String v;
  if      (currentValue >= 10000) v = String((long)currentValue);
  else if (currentValue >= 1000)  v = String(currentValue, 0);
  else if (currentValue >= 100)   v = String(currentValue, 1);
  else                            v = String(currentValue, 2);

  u8g2.clearBuffer();

  if (oledStyle == 1) {
    // ── Stil 1: GROSS – Label + großer Wert + Einheit ────────────────────────
    String lbl = meterLabel; lbl = utf8Clip(lbl, 10);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 7, lbl.c_str());
    u8g2.drawHLine(0, 9, 64);
    // Wert: 10x20 wenn <= 5 Zeichen, sonst 7x13
    if (v.length() <= 5) {
      u8g2.setFont(u8g2_font_10x20_tf);
      u8drawCenter(v.c_str(), 33);
    } else {
      u8g2.setFont(u8g2_font_7x13_tf);
      u8drawCenter(v.c_str(), 26);
    }
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 45, meterUnit.c_str());
    u8dot(fetchOk);

  } else if (oledStyle == 2) {
    // ── Stil 2: MINIMAL – nur Wert + Einheit, alles zentriert ────────────────
    u8g2.setFont(u8g2_font_7x13_tf);
    u8drawCenter(v.c_str(), 22);
    u8g2.setFont(u8g2_font_5x7_tf);
    String u = meterUnit; if (u.length() > 5) u = u.substring(0, 5);
    u8drawCenter(u.c_str(), 35);
    u8dot(fetchOk);

  } else if (oledStyle == 3) {
    // ── Stil 3: INVERTIERT – weißer Hintergrund, schwarzer Text ─────────────
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, 64, 48);           // weißes Rechteck
    u8g2.setDrawColor(0);                 // schwarze Schrift
    String lbl = meterLabel; lbl = utf8Clip(lbl, 10);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 7, lbl.c_str());
    u8g2.drawHLine(0, 9, 64);
    u8g2.setFont(u8g2_font_7x13_tf);
    u8drawCenter(v.c_str(), 25);
    u8g2.setFont(u8g2_font_baby_tf);
    u8g2.drawStr(0, 34, meterUnit.c_str());
    u8g2.drawStr(0, 41, lastReadingTime.c_str());
    u8g2.drawStr(0, 47, lastReadingDate.c_str());
    if (alarmActive) { u8g2.drawStr(36, 47, "!ALM"); }
    u8g2.setDrawColor(1);                 // zurücksetzen

  } else {
    // ── Stil 0: STANDARD ─────────────────────────────────────────────────────
    String lbl = meterLabel; lbl = utf8Clip(lbl, 10);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 7, lbl.c_str());
    u8g2.drawHLine(0, 9, 64);
    // Wert: 10x20 wenn <= 4 Zeichen (z.B. "2879"), sonst 7x13
    if (v.length() <= 4) {
      u8g2.setFont(u8g2_font_10x20_tf);
      u8drawCenter(v.c_str(), 27);
    } else {
      u8g2.setFont(u8g2_font_7x13_tf);
      u8drawCenter(v.c_str(), 24);
    }
    u8g2.setFont(u8g2_font_5x7_tf);
    String unit = meterUnit; if (unit.length() > 5) unit = unit.substring(0, 5);
    u8g2.drawStr(0, 37, unit.c_str());
    if (alarmActive) { u8g2.drawStr(30, 37, "!ALM"); }
    u8g2.setFont(u8g2_font_baby_tf);
    u8g2.drawStr(0, 42, lastReadingTime.c_str());
    u8g2.drawStr(0, 47, lastReadingDate.c_str());
    u8dot(fetchOk);
  }

  u8g2.sendBuffer();
}



// ─────────────────────────────────────────────────────────────────────────────
//  Carousel
// ─────────────────────────────────────────────────────────────────────────────
void saveCarousel() {
  prefs.begin("mm-car", false);
  prefs.putInt("count", carouselCount);
  prefs.putInt("sec",   carouselSec);
  prefs.putBool("act",  carouselActive);
  for (int i = 0; i < carouselCount; i++) {
    prefs.putString(("s"+String(i)).c_str(), carousel[i].sid);
    prefs.putString(("l"+String(i)).c_str(), carousel[i].label);
    prefs.putString(("u"+String(i)).c_str(), carousel[i].unit);
  }
  prefs.end();
}

void loadCarousel() {
  prefs.begin("mm-car", true);
  carouselCount  = prefs.getInt ("count", 0);
  carouselSec    = prefs.getInt ("sec",   10);
  carouselActive = prefs.getBool("act",   false);
  for (int i = 0; i < carouselCount && i < CAROUSEL_MAX; i++) {
    carousel[i].sid   = prefs.getString(("s"+String(i)).c_str(), "");
    carousel[i].label = prefs.getString(("l"+String(i)).c_str(), "");
    carousel[i].unit  = prefs.getString(("u"+String(i)).c_str(), "");
    carousel[i].val   = 0; carousel[i].ok = false;
  }
  prefs.end();
}

void carouselFetchCurrent() {
  if (carouselCount == 0) return;
  bool ok; String err;
  carousel[carouselIdx].val = fetchStateValue(carousel[carouselIdx].sid, ok, err);
  carousel[carouselIdx].ok  = ok;
  // Hauptwert auch aktualisieren (für Status-API)
  currentValue  = carousel[carouselIdx].val;
  meterLabel    = carousel[carouselIdx].label;
  meterUnit     = carousel[carouselIdx].unit;
  stateId       = carousel[carouselIdx].sid;
  fetchOk       = ok;
  statusMsg     = err;
  lastFetch     = millis();
  checkAlarm();
  oledValue();
}

void carouselNext() {
  if (carouselCount == 0) return;
  carouselIdx = (carouselIdx + 1) % carouselCount;
  carouselFetchCurrent();
  lastCarousel = millis();
}

void handleCarousel() {
  if (!carouselActive || carouselCount < 2) return;
  if (millis() - lastCarousel >= (unsigned long)carouselSec * 1000UL) {
    carouselNext();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  ioBroker Node-Registration & Config-Poll
//  Ab v0.4.0: Kommunikation direkt mit dem MeterMaster Adapter (Port adapterPort):
//    POST {adapterPort}/api/register          → Heartbeat + Node-Info
//    GET  {adapterPort}/api/nodes/{MAC}/config → Config vom Adapter holen
//    POST {adapterPort}/api/nodes/{MAC}/configAck → Config quittieren
//
//  simple-api (Port iobPort) wird nur noch für das Lesen der Zählerwerte genutzt.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
//  Log-Ringpuffer (Debug-Tab)
//  Speichert die letzten LOG_SIZE Einträge im RAM.
// ─────────────────────────────────────────────────────────────────────────────
#define LOG_SIZE 30

/** Ein Log-Eintrag mit Zeitstempel und Nachricht. */
struct LogEntry {
  unsigned long ts;   // millis() zum Zeitpunkt des Eintrags
  String        msg;  // Log-Nachricht
};

LogEntry logBuf[LOG_SIZE];
int logHead = 0;    // Index des nächsten Schreibplatzes (circular)
int logCount = 0;   // Anzahl bisher gespeicherter Einträge

/**
 * Fügt eine Nachricht in den Log-Ringpuffer ein.
 * Älteste Einträge werden überschrieben wenn der Puffer voll ist.
 *
 * @param msg  Die Log-Nachricht
 */
void addLog(const String& msg) {
  logBuf[logHead].ts  = millis();
  logBuf[logHead].msg = msg;
  logHead = (logHead + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;
  Serial.println("[LOG] " + msg);
}


void registerNode() {
  if (iobHost.isEmpty() || adapterPort == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  // JSON-Body aufbauen
  String body = String("{")
    + "\"mac\":\""     + nodeMac                      + "\","
    + "\"ip\":\""      + WiFi.localIP().toString()    + "\","
    + "\"name\":\""    + nodeName                     + "\","
    + "\"version\":\"" + FW_VERSION                   + "\""
    + "}";

  HTTPClient http;
  String url = "http://" + iobHost + ":" + String(adapterPort) + "/api/register";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  int code = http.POST(body);
  http.end();

  if (code == 200) {
    registerOk     = true;
    lastRegisterOk = millis();
    Serial.println("Node registered: " + nodeMac);
  } else {
    registerOk = false;
    addLog("Register fehlgeschlagen: HTTP " + String(code));
    Serial.println("Register failed: " + String(code));
  }
}

// Zusaetzlicher Heartbeat an iobroker.esp-hub (Port 8093) — nur fuer Dashboard-Sichtbarkeit.
// Die MeterMaster-Funktionen (Config, cmd, Zaehler) laufen weiter ueber adapterPort (8089).
void sendEspHubHeartbeat() {
  if (iobHost.isEmpty() || hubPort <= 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  DynamicJsonDocument doc(768);
  doc["mac"]        = nodeMac;
  doc["name"]       = nodeName;
  doc["hwType"]     = "esp32";
  doc["chipModel"]  = hwInfo.chipModel();
  doc["version"]    = FW_VERSION;
  doc["ip"]         = WiFi.localIP().toString();
  doc["rssi"]       = WiFi.RSSI();
  doc["uptime"]     = millis() / 1000UL;
  doc["freeHeap"]   = hwInfo.freeHeap();
  doc["freeSketch"] = hwInfo.freeSketchSpace();
  doc["hwSummary"]  = hwInfo.summaryLine();
  doc["module"]     = hwInfo.moduleGuess();
  {
    const int c0 = CpuLoad::core0Percent();
    const int c1 = CpuLoad::core1Percent();
    doc["cpu"] = (c1 >= 0) ? ((c0 + c1) / 2) : c0;
  }

  JsonObject ios = doc.createNestedObject("ios");
  JsonObject vIo = ios.createNestedObject("value");
  vIo["type"]  = "sensor";
  vIo["value"] = currentValue;
  vIo["unit"]  = meterUnit;
  JsonObject cIo = ios.createNestedObject("carousel");
  cIo["type"]  = "sensor";
  cIo["value"] = carouselActive ? carouselCount : 1;
  cIo["unit"]  = "";
  JsonObject aIo = ios.createNestedObject("adapter");
  aIo["type"]  = "sensor";
  aIo["value"] = registerOk ? 1 : 0;
  aIo["unit"]  = "";

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin("http://" + iobHost + ":" + String(hubPort) + "/api/register");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (code == 200) {
    hubOk     = true;
    lastHubOk = millis();
    StaticJsonDocument<256> rdoc;
    if (!deserializeJson(rdoc, resp)) {
      if (rdoc.containsKey("name") && !rdoc["name"].isNull()) {
        String newName = rdoc["name"].as<String>();
        newName.trim();
        if (newName.length() > 0 && newName != nodeName) {
          nodeName = newName;
          prefs.begin("mm", false);
          prefs.putString("nname", nodeName);
          prefs.end();
          Serial.println("[HB] Name: " + nodeName);
        }
      }
    }
  } else {
    hubOk = false;
  }
}

// Liest config vom MeterMaster Adapter – Adapter schreibt JSON rein
// Format: {"sid":"metermaster.0.X","label":"Strom","unit":"kWh"}
void pollConfig() {
  if (iobHost.isEmpty() || adapterPort == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://" + iobHost + ":" + String(adapterPort)
             + "/api/nodes/" + nodeMac + "/config";
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  if (code != 200) { http.end(); return; }
  String payload = http.getString();
  http.end();

  if (payload.isEmpty()) return;

  // Adapter antwortet: { "ok": true, "config": "<JSON-String oder null>", "cmd": "<JSON oder null>" }
  DynamicJsonDocument wrapper(512);
  if (deserializeJson(wrapper, payload)) return;
  if (!wrapper["ok"].as<bool>()) return;

  // ── CMD verarbeiten (einmalig, Adapter löscht danach) ──
  String cmdStr = wrapper["cmd"] | "";
  if (!cmdStr.isEmpty() && cmdStr != "null") {
    DynamicJsonDocument cmd(128);
    if (!deserializeJson(cmd, cmdStr)) {
      if (cmd.containsKey("ledOn")) {
        ledOn = cmd["ledOn"].as<bool>();
        digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
        addLog(String("CMD: LED ") + (ledOn ? "ein" : "aus"));
      }
      if (cmd.containsKey("sid")) {
        stateId    = cmd["sid"]   | stateId;
        meterLabel = cmd["label"] | meterLabel;
        meterUnit  = cmd["unit"]  | meterUnit;
        saveSettings();
        lastFetch = 0;
        addLog("CMD: Zähler geändert → " + meterLabel);
      }
    }
  }

  // ── Config verarbeiten ──
  String cfgStr = wrapper["config"] | "";
  if (cfgStr.isEmpty() || cfgStr == "null") return;

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, cfgStr)) return;

  // Leer-Config (sid == "") ignorieren
  String newSid = doc["sid"] | "";
  if (newSid.isEmpty() && !doc.containsKey("carousel")) return;

  bool changed = false;
  if (doc.containsKey("sid") && newSid != stateId) {
    stateId    = newSid;
    meterLabel = doc["label"] | meterLabel;
    meterUnit  = doc["unit"]  | meterUnit;
    changed    = true;
  }
  if (doc.containsKey("carouselSec")) {
    int s = doc["carouselSec"].as<int>();
    if (s > 0 && s != carouselSec) { carouselSec = s; changed = true; }
  }
  if (doc.containsKey("carouselActive")) {
    bool ca = doc["carouselActive"].as<bool>();
    if (ca != carouselActive) { carouselActive = ca; changed = true; }
  }
  if (doc.containsKey("carousel") && doc["carousel"].is<JsonArray>()) {
    JsonArray arr = doc["carousel"].as<JsonArray>();
    bool carouselDiff = arr.size() != (size_t)carouselCount;
    if (!carouselDiff) {
      for (size_t i = 0; i < arr.size() && !carouselDiff; i++) {
        JsonObject e = arr[i];
        carouselDiff = carousel[i].sid   != String(e["sid"]   | "")
                    || carousel[i].label != String(e["label"] | "")
                    || carousel[i].unit  != String(e["unit"]  | "");
      }
    }
    if (carouselDiff) {
      carouselCount = 0;
      for (JsonObject e : arr) {
        if (carouselCount >= CAROUSEL_MAX) break;
        carousel[carouselCount].sid   = e["sid"]   | "";
        carousel[carouselCount].label = e["label"] | "";
        carousel[carouselCount].unit  = e["unit"]  | "";
        carousel[carouselCount].val   = 0;
        carousel[carouselCount].ok    = false;
        carouselCount++;
      }
      changed = true;
    }
  }

  if (changed) {
    saveSettings();
    saveCarousel();
    lastFetch = 0;
    addLog("Config vom Adapter übernommen");
    Serial.println("Config vom Adapter übernommen");

    // ConfigAck an Adapter melden
    HTTPClient ackHttp;
    String ackUrl = "http://" + iobHost + ":" + String(adapterPort)
                  + "/api/nodes/" + nodeMac + "/configAck";
    ackHttp.begin(ackUrl);
    ackHttp.addHeader("Content-Type", "application/json");
    ackHttp.setTimeout(3000);
    String ackBody = String("{\"ack\":\"") + FW_VERSION + "@" + String(millis()) + "\"}";
    ackHttp.POST(ackBody);
    ackHttp.end();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────────────────────────────────────
void loadSettings() {
  prefs.begin("mm", true);
  iobHost        = prefs.getString("host",  "192.168.178.113");
  iobPort        = prefs.getInt   ("port",  8087);
  adapterPort    = prefs.getInt   ("aport", 8089);
  hubPort        = prefs.getInt   ("hport", 8093);
  stateId        = prefs.getString("sid",   "metermaster.0.MeinHaus.Westerheim.Warmwasser.readings.latest");
  meterLabel     = prefs.getString("lbl",   "Warmwasser");
  meterUnit      = prefs.getString("unit",  "m³");
  fetchSec       = prefs.getInt   ("intv",  60);
  alarmEnabled   = prefs.getBool  ("alEn",  false);
  alarmThreshold = prefs.getFloat ("alThr", 100.0);
  alarmAbove     = prefs.getBool  ("alAb",  true);
  oledStyle      = prefs.getInt   ("oStyl", 0);
  nodeName       = prefs.getString("nname", nodeName);
  prefs.end();
}
void saveSettings() {
  prefs.begin("mm", false);
  prefs.putString("host", iobHost);
  prefs.putInt   ("port", iobPort);
  prefs.putInt   ("aport",adapterPort);
  prefs.putInt   ("hport", hubPort);
  prefs.putString("sid",  stateId);
  prefs.putString("lbl",  meterLabel);
  prefs.putString("unit", meterUnit);
  prefs.putInt   ("intv", fetchSec);
  prefs.putBool  ("alEn", alarmEnabled);
  prefs.putFloat ("alThr",alarmThreshold);
  prefs.putBool  ("alAb", alarmAbove);
  prefs.putInt   ("oStyl",oledStyle);
  prefs.putString("nname", nodeName);
  prefs.end();
}

// ─────────────────────────────────────────────────────────────────────────────
//  ioBroker Datenabruf
// ─────────────────────────────────────────────────────────────────────────────
float fetchStateValue(const String& sid, bool& ok, String& errMsg) {
  if (WiFi.status() != WL_CONNECTED) { ok=false; errMsg="WLAN getrennt"; return 0; }
  HTTPClient http;
  http.begin("http://"+iobHost+":"+String(iobPort)+"/get/"+sid);
  http.setTimeout(5000);
  int code = http.GET();
  float val = 0;
  if (code == 200) {
    StaticJsonDocument<64> filter;
    filter["val"]=true; filter["ts"]=true;
    DynamicJsonDocument doc(256);
    DeserializationError err = deserializeJson(doc, http.getString(),
                                DeserializationOption::Filter(filter));
    if (!err) {
      JsonVariant s; if(doc.is<JsonArray>()) s=doc[0]; else s=doc.as<JsonVariant>();
      val = s["val"].as<float>();
      if (s.containsKey("ts")) formatTs(s["ts"].as<long long>());
      else { lastReadingTime=nowTimeStr(); lastReadingDate=nowDateStr(); }
      ok=true; errMsg="OK";
    } else { ok=false; errMsg="JSON: "+String(err.c_str()); }
  } else if(code<0) { ok=false; errMsg="Keine Verbindung"; }
  else              { ok=false; errMsg="HTTP "+String(code); }
  http.end();
  return val;
}

void checkAlarm() {
  bool triggered = alarmEnabled && (
    alarmAbove  ? (currentValue > alarmThreshold)
                : (currentValue < alarmThreshold)
  );
  alarmActive = triggered;
}

void doFetch() {
  if (carouselActive && carouselCount > 0) {
    carouselFetchCurrent();
  } else {
    currentValue = fetchStateValue(stateId, fetchOk, statusMsg);
    lastFetch    = millis();
    checkAlarm();
    oledValue();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  LED
// ─────────────────────────────────────────────────────────────────────────────
void setLed(bool on) {
  ledOn = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void handleBlink() {
  if (!alarmActive) { ledBlinking=false; return; }
  ledBlinking = true;
  if (millis()-lastBlink > 500) {
    lastBlink = millis();
    setLed(!ledOn);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Web-UI: web/index.html → tools/pack_ui.py → src/web/UiPagesGz.h (gzip PROGMEM)

void hApiVersion() {
  server.send(200,"application/json",
    String("{") +
    "\"version\":\"" + String(FW_VERSION) + "\"," +
    "\"build\":\"" + String(__DATE__) + " " + String(__TIME__) + "\"," +
    "\"ip\":\""     + WiFi.localIP().toString() + "\"," +
    "\"ghUser\":\"" + String(GH_USER) + "\"," +
    "\"ghRepoEsp\":\"" + String(GH_REPO_ESP) + "\"," +
    "\"ghRepoIob\":\"" + String(GH_REPO_IOB) + "\"}");
}


// ─────────────────────────────────────────────────────────────────────────────
//  /api/nodeinfo  – für ioBroker-Adapter (liest alle Node-Infos)
// ─────────────────────────────────────────────────────────────────────────────
void hApiNodeinfo() {
  String carArr = "[";
  for (int i = 0; i < carouselCount; i++) {
    if (i) carArr += ",";
    carArr += String("{\"sid\":\"") + carousel[i].sid + "\""
            + ",\"label\":\"" + carousel[i].label + "\""
            + ",\"unit\":\"" + carousel[i].unit + "\""
            + ",\"val\":" + String(carousel[i].val, 3)
            + ",\"ok\":" + String(carousel[i].ok?"true":"false") + "}";
  }
  carArr += "]";
  server.send(200,"application/json",
    String("{") +
    "\"mac\":\"" + nodeMac + "\"," +
    "\"name\":\"" + nodeName + "\"," +
    "\"version\":\"" + FW_VERSION + "\"," +
    "\"ip\":\"" + WiFi.localIP().toString() + "\"," +
    "\"uptime\":" + String(millis()/1000) + "," +
    "\"rssi\":" + String(WiFi.RSSI()) + "," +
    "\"stateId\":\"" + stateId + "\"," +
    "\"label\":\"" + meterLabel + "\"," +
    "\"unit\":\"" + meterUnit + "\"," +
    "\"value\":" + String(currentValue,4) + "," +
    "\"fetchOk\":" + String(fetchOk?"true":"false") + "," +
    "\"carouselActive\":" + String(carouselActive?"true":"false") + "," +
    "\"carouselIdx\":" + String(carouselIdx) + "," +
    "\"carouselSec\":" + String(carouselSec) + "," +
    "\"carousel\":" + carArr + "}");
}

// ─────────────────────────────────────────────────────────────────────────────
//  /api/carousel  GET+POST
// ─────────────────────────────────────────────────────────────────────────────
void hApiCarouselGet() {
  String r = String("{\"active\":") + String(carouselActive?"true":"false")
           + ",\"interval\":" + String(carouselSec)
           + ",\"current\":" + String(carouselIdx)
           + ",\"entries\":[";
  for (int i = 0; i < carouselCount; i++) {
    if (i) r += ",";
    r += String("{\"sid\":\"") + carousel[i].sid + "\",\"label\":\"" + carousel[i].label + "\",\"unit\":\"" + carousel[i].unit + "\"}";
  }
  r += "]}";
  server.send(200,"application/json",r);
}

void hApiCarouselPost() {
  if (!server.hasArg("plain")) { server.send(400,"application/json",String("{\"ok\":false}")); return; }
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc,server.arg("plain"))) { server.send(400,"application/json","{\"ok\":false,\"msg\":\"JSON\"}"); return; }
  if (!doc["active"].isNull()) carouselActive = doc["active"].as<bool>();
  if (!doc["interval"].isNull()) carouselSec = max(3, doc["interval"].as<int>());
  if (doc["entries"].is<JsonArray>()) {
    carouselCount = 0; carouselIdx = 0;
    for (JsonObject e : doc["entries"].as<JsonArray>()) {
      if (carouselCount >= CAROUSEL_MAX) break;
      carousel[carouselCount].sid   = e["sid"]   | "";
      carousel[carouselCount].label = e["label"] | "";
      carousel[carouselCount].unit  = e["unit"]  | "";
      carousel[carouselCount].val   = 0; carousel[carouselCount].ok = false;
      carouselCount++;
    }
  }
  saveCarousel(); lastFetch = 0; lastCarousel = millis();
  server.send(200,"application/json",String("{\"ok\":true}")); 
}

void hApiCarouselNext() {
  carouselNext();
  server.send(200,"application/json",
    String("{\"ok\":true,\"idx\":") + String(carouselIdx) + ",\"label\":\"" + meterLabel + "\"}");
}

void hApiNodeName() {
  if (server.hasArg("name")) { nodeName = server.arg("name"); }
  server.send(200,"application/json",String("{\"ok\":true,\"name\":\"") + nodeName + String("\"}"));
}

void hRoot() {
  server.sendHeader(F("Content-Encoding"), F("gzip"));
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send_P(200, "text/html", (PGM_P)PAGE_MAIN_GZ, PAGE_MAIN_GZ_LEN);
}

void hApiStatus() {
  String ago    = (lastFetch==0)?"Nie":(String((millis()-lastFetch)/1000)+" s");
  String regAgo = (lastRegisterOk==0)?"Nie":(String((millis()-lastRegisterOk)/1000)+" s");
  String hubAgo = (lastHubOk==0)?"Nie":(String((millis()-lastHubOk)/1000)+" s");
  server.send(200,"application/json",
    "{\"ok\":"            + String(fetchOk?"true":"false")
    +",\"value\":"        + String(currentValue,4)
    +",\"unit\":\""       + meterUnit   +"\""
    +",\"label\":\""      + meterLabel  +"\""
    +",\"status\":\""     + statusMsg   +"\""
    +",\"lastFetch\":\""  + ago         +"\""
    +",\"stateId\":\""    + stateId     +"\""
    +",\"readingTime\":\"" + lastReadingTime +"\""
    +",\"readingDate\":\"" + lastReadingDate +"\""
    +",\"ledOn\":"         + String(ledOn?"true":"false")
    +",\"alarmActive\":"   + String(alarmActive?"true":"false")
    +",\"alarmEnabled\":"  + String(alarmEnabled?"true":"false")
    +",\"alarmThreshold\":"+ String(alarmThreshold,2)
    +",\"alarmAbove\":"    + String(alarmAbove?"true":"false")
    +",\"registerOk\":"    + String(registerOk?"true":"false")
    +",\"hubOk\":"         + String(hubOk?"true":"false")
    +",\"hubPort\":"       + String(hubPort)
    +",\"lastRegisterAgo\":\"" + regAgo + "\""
    +",\"lastHubAgo\":\""   + hubAgo + "\""
    +",\"ip\":\""          + WiFi.localIP().toString() +"\"}");
}

void hApiGetSettings() {
  server.send(200,"application/json",
    "{\"iobHost\":\""     + iobHost   +"\""
    +",\"iobPort\":"      + String(iobPort)
    +",\"adapterPort\":"  + String(adapterPort)
    +",\"hubPort\":"      + String(hubPort)
    +",\"stateId\":\""    + stateId   +"\""
    +",\"label\":\""      + meterLabel+"\""
    +",\"unit\":\""       + meterUnit +"\""
    +",\"interval\":"     + String(fetchSec)
    +",\"alarmEnabled\":"  + String(alarmEnabled?"true":"false")
    +",\"alarmThreshold\":"+ String(alarmThreshold,2)
    +",\"alarmAbove\":"    + String(alarmAbove?"true":"false") +"}");
}

void hApiPostSettings() {
  if (!server.hasArg("plain")) { server.send(400,"application/json","{\"ok\":false,\"msg\":\"Kein Body\"}"); return; }
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc,server.arg("plain"))) { server.send(400,"application/json","{\"ok\":false,\"msg\":\"JSON\"}"); return; }
  if (!doc["iobHost"].isNull()) iobHost      = doc["iobHost"].as<String>();
  if (!doc["iobPort"].isNull()) iobPort      = doc["iobPort"].as<int>();
  if (!doc["adapterPort"].isNull()) adapterPort = doc["adapterPort"].as<int>();
  if (!doc["hubPort"].isNull()) hubPort = doc["hubPort"].as<int>();
  if (!doc["stateId"].isNull()) stateId    = doc["stateId"].as<String>();
  if (!doc["label"].isNull())   meterLabel = doc["label"].as<String>();
  if (!doc["unit"].isNull())    meterUnit  = doc["unit"].as<String>();
  if (!doc["interval"].isNull())fetchSec   = doc["interval"].as<int>();
  saveSettings(); lastFetch=0;
  server.send(200,"application/json","{\"ok\":true}");
}

void hApiPostAlarm() {
  if (!server.hasArg("plain")) { server.send(400,"application/json","{\"ok\":false,\"msg\":\"Kein Body\"}"); return; }
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc,server.arg("plain"))) { server.send(400,"application/json","{\"ok\":false,\"msg\":\"JSON\"}"); return; }
  alarmEnabled   = doc["alarmEnabled"].as<bool>();
  alarmThreshold = doc["alarmThreshold"].as<float>();
  alarmAbove     = doc["alarmAbove"].as<bool>();
  saveSettings();
  checkAlarm();
  server.send(200,"application/json","{\"ok\":true}");
}

void hApiTestConn() {
  if (!server.hasArg("plain")) { server.send(400,"application/json","{\"ok\":false,\"msg\":\"Kein Body\"}"); return; }
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc,server.arg("plain"))) { server.send(400,"application/json","{\"ok\":false,\"msg\":\"JSON\"}"); return; }
  String th=doc["iobHost"].as<String>(); int tp=doc["iobPort"].as<int>(); String ts=doc["stateId"].as<String>();
  if (WiFi.status()!=WL_CONNECTED){server.send(200,"application/json","{\"ok\":false,\"msg\":\"WLAN getrennt\"}");return;}
  HTTPClient http;
  http.begin("http://"+th+":"+String(tp)+"/get/"+ts); http.setTimeout(5000);
  int code=http.GET(); float val=0; bool ok=false; String msg;
  if(code==200){
    StaticJsonDocument<64> f; f["val"]=true; f["ts"]=true;
    DynamicJsonDocument resp(256);
    if(!deserializeJson(resp,http.getString(),DeserializationOption::Filter(f))){
      JsonVariant sv; if(resp.is<JsonArray>())sv=resp[0];else sv=resp.as<JsonVariant>();
      val=sv["val"].as<float>(); ok=true; msg="OK";
    } else msg="JSON-Fehler";
  } else if(code<0) msg="Keine Verbindung zu "+th+":"+String(tp);
  else msg="HTTP "+String(code);
  http.end();
  server.send(200,"application/json",
    "{\"ok\":"+String(ok?"true":"false")+",\"value\":"+String(val,4)+",\"msg\":\""+msg+"\"}");
}

void hApiTestAdapter() {
  // Testet die Verbindung zum MeterMaster Adapter (Port adapterPort)
  // via GET /api/version – kein Auth nötig
  int tp = adapterPort;
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(128);
    if (!deserializeJson(doc, server.arg("plain")))
      if (!doc["adapterPort"].isNull()) tp = doc["adapterPort"].as<int>();
  }
  if (WiFi.status() != WL_CONNECTED) {
    server.send(200, "application/json", "{\"ok\":false,\"msg\":\"WLAN getrennt\"}"); return;
  }
  HTTPClient http;
  http.begin("http://" + iobHost + ":" + String(tp) + "/api/version");
  http.setTimeout(5000);
  int code = http.GET();
  String ver = "?";
  bool ok = false;
  String msg;
  if (code == 200) {
    StaticJsonDocument<64> f; f["current"] = true;
    DynamicJsonDocument resp(256);
    if (!deserializeJson(resp, http.getString(), DeserializationOption::Filter(f))) {
      ver = resp["current"] | "?";
      ok = true; msg = "Adapter v" + ver + " erreichbar";
    } else { msg = "JSON-Fehler"; }
  } else if (code < 0) {
    msg = "Keine Verbindung zu " + iobHost + ":" + String(tp);
  } else {
    msg = "HTTP " + String(code);
  }
  http.end();
  server.send(200, "application/json",
    "{\"ok\":" + String(ok?"true":"false") + ",\"version\":\"" + ver + "\",\"msg\":\"" + msg + "\"}");
}

void hApiTestHub() {
  int tp = hubPort;
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(128);
    if (!deserializeJson(doc, server.arg("plain")))
      if (!doc["hubPort"].isNull()) tp = doc["hubPort"].as<int>();
  }
  if (tp <= 0) {
    server.send(200, "application/json", "{\"ok\":false,\"msg\":\"ESP-Hub deaktiviert (Port 0)\"}"); return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    server.send(200, "application/json", "{\"ok\":false,\"msg\":\"WLAN getrennt\"}"); return;
  }
  HTTPClient http;
  http.begin("http://" + iobHost + ":" + String(tp) + "/api/version");
  http.setTimeout(5000);
  int code = http.GET();
  String ver = "?";
  bool ok = false;
  String msg;
  if (code == 200) {
    StaticJsonDocument<64> f; f["current"] = true;
    DynamicJsonDocument resp(256);
    if (!deserializeJson(resp, http.getString(), DeserializationOption::Filter(f))) {
      ver = resp["current"] | "?";
      ok = true; msg = "ESP-Hub v" + ver + " erreichbar";
    } else { msg = "JSON-Fehler"; }
  } else if (code < 0) {
    msg = "Keine Verbindung zu " + iobHost + ":" + String(tp);
  } else {
    msg = "HTTP " + String(code);
  }
  http.end();
  server.send(200, "application/json",
    "{\"ok\":" + String(ok?"true":"false") + ",\"version\":\"" + ver + "\",\"msg\":\"" + msg + "\"}");
}

void hApiOled() {
  if (!server.hasArg("plain")) { server.send(400,"application/json","{\"ok\":false,\"msg\":\"Kein Body\"}"); return; }
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc,server.arg("plain"))) { server.send(400,"application/json","{\"ok\":false,\"msg\":\"JSON\"}"); return; }
  stateId=doc["stateId"].as<String>(); meterLabel=doc["label"].as<String>(); meterUnit=doc["unit"].as<String>();
  if(!doc["oledStyle"].isNull()) oledStyle=doc["oledStyle"].as<int>();
  saveSettings(); lastFetch=0; oledValue();
  server.send(200,"application/json","{\"ok\":true}");
}

static void appendDiscoverState(String &r, bool &first, const String &sid, float val, String lbl, String unit) {
  String escSid = sid; escSid.replace("\"","\\\"");
  String escLbl = lbl; escLbl.replace("\"","\\\"");
  String escUnit = unit; escUnit.replace("\"","\\\"");
  if (!first) r += ",";
  r += "{\"id\":\"" + escSid + "\",\"val\":" + String(val, 3) + ",\"label\":\"" + escLbl + "\",\"unit\":\"" + escUnit + "\"}";
  first = false;
}

void hApiDiscover() {
  if (WiFi.status()!=WL_CONNECTED){server.send(503,"application/json","{\"ok\":false,\"msg\":\"WLAN getrennt\"}");return;}
  if (ESP.getFreeHeap()<10000){server.send(200,"application/json",
    "{\"ok\":false,\"msg\":\"Heap zu knapp ("+String(ESP.getFreeHeap()/1024)+" KB)\"}");return;}

  HTTPClient http;
  if (adapterPort > 0) {
    http.begin("http://" + iobHost + ":" + String(adapterPort) + "/api/discover");
    http.setTimeout(8000);
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      http.end();
      DynamicJsonDocument doc(8192);
      if (!deserializeJson(doc, body) && doc.is<JsonArray>()) {
        String r = "{\"ok\":true,\"states\":[";
        bool first = true;
        for (JsonObject item : doc.as<JsonArray>()) {
          String sid = item["stateId"] | "";
          if (sid.isEmpty()) continue;
          float val = item["latest"].isNull() ? 0 : item["latest"].as<float>();
          String meter = item["meter"] | item["label"] | "";
          String house = item["house"] | "";
          String apt = item["apartment"] | "";
          String lbl = house;
          if (!apt.isEmpty()) { if (!lbl.isEmpty()) lbl += " · "; lbl += apt; }
          if (!meter.isEmpty()) { if (!lbl.isEmpty()) lbl += " · "; lbl += meter; }
          appendDiscoverState(r, first, sid, val, lbl, item["unit"] | "");
        }
        r += "]}";
        server.send(200, "application/json", r);
        return;
      }
    } else {
      http.end();
    }
  }

  http.begin("http://" + iobHost + ":" + String(iobPort) + "/getStates/metermaster.0.*.readings.latest");
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) {
    http.end();
    server.send(200, "application/json",
      "{\"ok\":false,\"msg\":\"HTTP " + String(code) + " – simple-api aktiv?\"}");
    return;
  }
  String body = http.getString();
  http.end();
  DynamicJsonDocument doc(8192);
  doc.clear();
  if (deserializeJson(doc, body)) {
    server.send(200, "application/json", "{\"ok\":false,\"msg\":\"JSON-Fehler\"}");
    return;
  }
  String r = "{\"ok\":true,\"states\":[";
  bool first = true;
  for (JsonPair kv : doc.as<JsonObject>()) {
    String sid = kv.key().c_str();
    JsonVariant sv = kv.value();
    if (sv["val"].isNull()) continue;
    if (!sv["val"].is<float>() && !sv["val"].is<int>()) continue;
    float val = sv["val"].as<float>();
    String lbl = sid;
    int d2 = sid.indexOf('.', sid.indexOf('.') + 1);
    if (d2 >= 0) lbl = sid.substring(d2 + 1);
    lbl.replace(".", " · ");
    String unit = "";
    String sl = sid;
    sl.toLowerCase();
    if (sl.indexOf("strom") >= 0 || sl.indexOf("kwh") >= 0) unit = "kWh";
    else if (sl.indexOf("gas") >= 0) unit = "m³";
    else if (sl.indexOf("wasser") >= 0 || sl.indexOf("water") >= 0) unit = "m³";
    else if (sl.indexOf("waerm") >= 0) unit = "kWh";
    appendDiscoverState(r, first, sid, val, lbl, unit);
  }
  r += "]}";
  server.send(200, "application/json", r);
}



// POST /api/ota-github – Lädt eine .bin von einer URL und installiert sie per OTA.
// Body: {"url":"https://...bin","version":"v0.3.0"}
void hApiOtaGithub() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", String("{") + "\"ok\":false,\"msg\":\"Kein Body\"}"); return;
  }
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", String("{") + "\"ok\":false,\"msg\":\"JSON-Fehler\"}");
    return;
  }
  String url = doc["url"].as<String>();
  String ver = doc["version"] | String("?");
  if (!url.startsWith("http")) {
    server.send(400, "application/json", String("{") + "\"ok\":false,\"msg\":\"Ungueltige URL\"}");
    return;
  }

  // Sofort antworten – OTA blockiert den WebServer
  server.send(200, "application/json", String("{") + "\"ok\":true,\"msg\":\"OTA gestartet\"}");
  addLog("OTA GitHub: lade " + ver + " von " + url);

  delay(200);  // Antwort senden lassen

  HTTPClient http;
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(30000);
  int code = http.GET();
  if (code != 200) {
    addLog("OTA HTTP-Fehler: " + String(code));
    http.end();
    return;
  }

  int len = http.getSize();
  if (!Update.begin(len > 0 ? len : UPDATE_SIZE_UNKNOWN)) {
    addLog("OTA begin() Fehler");
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  http.end();

  if (Update.end() && Update.isFinished()) {
    addLog("OTA erfolgreich: " + String(written) + " Bytes. Neustart…");
    delay(500);
    ESP.restart();
  } else {
    addLog("OTA fehlgeschlagen: " + String(Update.getError()));
  }
}

// GET /api/restart – Startet den ESP32 neu (nach 500 ms Verzögerung).
void hApiRestart() {
  server.send(200, "application/json", String("{") + "\"ok\":true,\"msg\":\"Neustart in 500ms\"}");
  delay(500);
  ESP.restart();
}

// GET /api/log – Gibt die letzten LOG_SIZE Log-Einträge als JSON-Array zurück.
void hApiLog() {
  String r = "[";
  int count = min(logCount, LOG_SIZE);
  for (int i = 0; i < count; i++) {
    int idx = (logHead - count + i + LOG_SIZE) % LOG_SIZE;
    if (i) r += ",";
    String m = logBuf[idx].msg;
    m.replace("\\", "\\\\"); m.replace("\"", "\\\"");
    r += String("{\"ts\":") + String(logBuf[idx].ts) + ",\"msg\":\"" + m + "\"}";
  }
  r += "]";
  server.send(200, "application/json", r);
}

void hApiLed() {
  if(server.hasArg("state")){
    String s=server.arg("state");
    if(s=="on") setLed(true); else if(s=="off") setLed(false); else if(s=="toggle") setLed(!ledOn);
  }
  server.send(200,"application/json",
    String("{\"ok\":true,\"ledOn\":")+String(ledOn?"true":"false")+"}");
}

void hApiSysinfo() {
  int32_t rssi=WiFi.RSSI();
  int q=(rssi>=-50)?100:(rssi<=-100)?0:2*(rssi+100);
  String j = "{";
  j += "\"ssid\":\"" + WiFi.SSID() + "\"";
  j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"mac\":\"" + WiFi.macAddress() + "\"";
  j += ",\"rssi\":" + String(rssi);
  j += ",\"quality\":" + String(q);
  j += ",\"channel\":" + String(WiFi.channel());
  j += ",\"heapFree\":" + String(hwInfo.freeHeap());
  j += ",\"heap\":" + String(hwInfo.freeHeap());
  j += ",\"heapTotal\":" + String(ESP.getHeapSize());
  j += ",\"sketchUsed\":" + String(hwInfo.sketchSize());
  j += ",\"sketchFree\":" + String(hwInfo.freeSketchSpace());
  j += ",\"sketch\":" + String(hwInfo.freeSketchSpace());
  j += ",\"chipModel\":\"" + hwInfo.chipModel() + "\"";
  j += ",\"chip\":\"" + hwInfo.chipModel() + "\"";
  j += ",\"version\":\"" FW_VERSION "\"";
  j += ",\"name\":\"" + nodeName + "\"";
  j += ",\"cpuFreq\":" + String(ESP.getCpuFreqMHz());
  j += ",\"uptime\":" + String(millis()/1000);
  j += ",\"ntpSynced\":" + String(ntpSynced?"true":"false");
  j += ",\"nowTime\":\"" + nowTimeStr() + "\"";
  j += ",\"nowDate\":\"" + nowDateStr() + "\"";
  hwInfo.appendStatusFields(j);
  CpuLoad::appendStatusFields(j);
  j += "}";
  server.send(200,"application/json",j);
}

void hOtaUpload() {
  HTTPUpload& up=server.upload();
  if(up.status==UPLOAD_FILE_START){oledClear("OTA Update","Starte...");if(!Update.begin(UPDATE_SIZE_UNKNOWN))Update.printError(Serial);}
  else if(up.status==UPLOAD_FILE_WRITE){if(Update.write(up.buf,up.currentSize)!=up.currentSize)Update.printError(Serial);
    oledClear("OTA Update",("Schreibe "+String(Update.progress()*100/max((size_t)1,Update.progress()+Update.remaining()))+"%").c_str());}
  else if(up.status==UPLOAD_FILE_END){if(Update.end(true))oledClear("OTA OK","Neustart...");else{Update.printError(Serial);oledClear("OTA FEHLER","");}}
}
void hOtaFinish(){
  server.sendHeader("Connection","close");
  if(Update.hasError())server.send(500,"text/plain","Update fehlgeschlagen");
  else{server.send(200,"text/plain","OK");delay(500);ESP.restart();}
}

// ─────────────────────────────────────────────────────────────────────────────
//  Setup
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);  // LED aus

  hwInfo.probe();
  hwInfo.logToSerial();
  CpuLoad::begin();

  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();
  u8g2.setContrast(255);
  oledClear("MeterMaster", "Node v" FW_VERSION, "Starte...");

  loadSettings();

  WiFiManager wm;
  wm.setConfigPortalTimeout(120);
  wm.setAPCallback([](WiFiManager*){ oledClear("WLAN Setup","AP:MeterMaster","-Setup"); });
  wm.setSaveConfigCallback([](){ oledClear("WLAN OK","Verbunden!",""); });
  if (!wm.autoConnect("MeterMaster-Setup","")) {
    oledClear("WLAN","Timeout.","Neustart..."); delay(2000); ESP.restart();
  }

  oledClear("NTP sync...","","");
  syncNTP();
  Serial.println(ntpSynced?"NTP: OK":"NTP: Fehler");

  String heapStr = String(ESP.getFreeHeap()/1024)+" KB frei";
  oledClear("IP:", WiFi.localIP().toString().c_str(), heapStr.c_str());
  delay(2000);

  server.on("/",             HTTP_GET,  hRoot);
  server.on("/api/version",   HTTP_GET,  hApiVersion);
  server.on("/api/status",   HTTP_GET,  hApiStatus);
  server.on("/api/settings", HTTP_GET,  hApiGetSettings);
  server.on("/api/settings", HTTP_POST, hApiPostSettings);
  server.on("/api/alarm",    HTTP_POST, hApiPostAlarm);
  server.on("/api/test",        HTTP_POST, hApiTestConn);
  server.on("/api/testadapter", HTTP_POST, hApiTestAdapter);
  server.on("/api/testhub",      HTTP_POST, hApiTestHub);
  server.on("/api/oled",     HTTP_POST, hApiOled);
  server.on("/api/discover", HTTP_GET,  hApiDiscover);
  server.on("/api/led",      HTTP_GET,  hApiLed);
  server.on("/api/sysinfo",  HTTP_GET,  hApiSysinfo);
  server.on("/api/nodeinfo", HTTP_GET,  hApiNodeinfo);
  server.on("/api/carousel", HTTP_GET,  hApiCarouselGet);
  server.on("/api/carousel", HTTP_POST, hApiCarouselPost);
  server.on("/api/carousel/next", HTTP_GET, hApiCarouselNext);
  server.on("/api/nodename", HTTP_GET,  hApiNodeName);
  server.on("/api/log",      HTTP_GET,  hApiLog);
  server.on("/api/restart",  HTTP_GET,  hApiRestart);
  server.on("/api/ota-github", HTTP_POST, hApiOtaGithub);
  server.on("/update",       HTTP_POST, hOtaFinish, hOtaUpload);

  nodeMac = WiFi.macAddress();
  nodeMac.replace(":", "");
  addLog("Gestart. IP: " + WiFi.localIP().toString() + " MAC: " + nodeMac);
  loadCarousel();
  registerNode();
  sendEspHubHeartbeat();
  lastRegister     = millis();
  lastHubHeartbeat = millis();
  lastConfigPoll = millis();
  server.begin();
  doFetch();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();
  CpuLoad::poll();
  // Fetch
  if (millis()-lastFetch >= (unsigned long)fetchSec*1000UL) doFetch();
  // Carousel
  handleCarousel();
  // ioBroker Heartbeat
  if (millis()-lastRegister >= REGISTER_INTERVAL) {
    registerNode();
    sendEspHubHeartbeat();
    lastRegister = millis();
    lastHubHeartbeat = millis();
  }
  // Config-Poll (ioBroker → ESP32)
  if (millis()-lastConfigPoll >= CONFIGPOLL_INTERVAL) {
    pollConfig(); lastConfigPoll = millis();
  }
  // Alarm LED
  if (!alarmActive) {
    if (ledBlinking) { ledBlinking=false; setLed(false); }
  } else {
    handleBlink();
  }
}
