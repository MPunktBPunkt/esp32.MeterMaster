/*
 * ============================================================
 *  MeterMaster ESP32 Display Node  –  v0.2.0
 *  Hardware: ESP32 D1 Mini + 64×48 OLED (SSD1306, I²C)
 * ============================================================
 *
 *  Libraries (Arduino Library Manager):
 *    - WiFiManager          by tzapu       >= 2.0.17
 *    - Adafruit SSD1306     by Adafruit    >= 2.5
 *    - Adafruit GFX Library by Adafruit    >= 1.11
 *    - ArduinoJson          by Blanchon    >= 6.21
 *
 *  NEU in v1.5:
 *    - Carousel: automatisch durch mehrere Zähler blättern
 *    - Node-Registrierung: ESP32 meldet sich bei ioBroker an
 *    - Config-Polling: ioBroker kann OLED-Zähler fernsteuern
 *    - /api/nodeinfo Endpunkt für den ioBroker-Adapter
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
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
// ── Hardware ──────────────────────────────────────────────────────────────────
#define FW_VERSION  "0.2.0"
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

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
WebServer server(80);
Preferences prefs;

// ── Persistente Einstellungen ─────────────────────────────────────────────────
String iobHost      = "192.168.178.113";
int    iobPort      = 8087;
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
String nodeMac       = "";   // gesetzt nach WiFi-Verbindung
String nodeName      = "MeterMaster Node";
unsigned long lastRegister  = 0;
unsigned long lastConfigPoll= 0;
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
//  Layout:
//    y= 0..7  Zeile 1: Label
//    y= 9     Trennlinie
//    y=11..18 Zeile 2: Wert (zentriert)
//    y=20..27 Zeile 3: Einheit
//    y=29..36 Zeile 4: Uhrzeit
//    y=38..45 Zeile 5: Datum  +  Status-Punkt rechts (y=42)
// ─────────────────────────────────────────────────────────────────────────────
void oledClear(const char* l1="", const char* l2="", const char* l3="") {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0,  0); oled.print(l1);
  oled.setCursor(0, 18); oled.print(l2);
  oled.setCursor(0, 34); oled.print(l3);
  oled.display();
}

void oledValue() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  // Wert-String aufbereiten
  String v;
  if      (currentValue >= 10000) v = String((long)currentValue);
  else if (currentValue >= 1000)  v = String(currentValue, 0);
  else if (currentValue >= 100)   v = String(currentValue, 1);
  else                            v = String(currentValue, 2);

  if (oledStyle == 1) {
    // ── Stil 1: GROSS – Wert mit textSize 2, kein Datum ──────────────────────
    oled.setTextSize(1);
    String lbl = meterLabel; if(lbl.length()>9) lbl=lbl.substring(0,9);
    oled.setCursor(0,0); oled.print(lbl);
    oled.drawLine(0,9,63,9,SSD1306_WHITE);
    // Wert textSize 2 wenn kurz genug
    uint8_t ts = (v.length()<=4) ? 2 : 1;
    oled.setTextSize(ts);
    int16_t bx,by; uint16_t bw,bh;
    oled.getTextBounds(v.c_str(),0,0,&bx,&by,&bw,&bh);
    oled.setCursor(max(0,(int)(64-(int)bw)/2), (ts==2)?12:18);
    oled.print(v);
    oled.setTextSize(1);
    oled.setCursor(0,40); oled.print(meterUnit);
    oled.setCursor(0,40+8); // no space, skip
    if(fetchOk) oled.fillCircle(61,42,3,SSD1306_WHITE);
    else        oled.drawCircle(61,42,3,SSD1306_WHITE);

  } else if (oledStyle == 2) {
    // ── Stil 2: MINIMAL – nur Wert + Einheit zentriert ───────────────────────
    oled.setTextSize(1);
    int16_t bx,by; uint16_t bw,bh;
    oled.getTextBounds(v.c_str(),0,0,&bx,&by,&bw,&bh);
    oled.setCursor(max(0,(int)(64-(int)bw)/2),14);
    oled.print(v);
    String u = meterUnit; if(u.length()>4)u=u.substring(0,4);
    oled.getTextBounds(u.c_str(),0,0,&bx,&by,&bw,&bh);
    oled.setCursor(max(0,(int)(64-(int)bw)/2),26);
    oled.print(u);
    if(fetchOk) oled.fillCircle(61,42,3,SSD1306_WHITE);
    else        oled.drawCircle(61,42,3,SSD1306_WHITE);

  } else if (oledStyle == 3) {
    // ── Stil 3: INVERTIERT – weißer Hintergrund ──────────────────────────────
    oled.fillRect(0,0,64,48,SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    oled.setTextSize(1);
    String lbl = meterLabel; if(lbl.length()>9) lbl=lbl.substring(0,9);
    oled.setCursor(0,0); oled.print(lbl);
    oled.drawLine(0,9,63,9,SSD1306_BLACK);
    int16_t bx,by; uint16_t bw,bh;
    oled.getTextBounds(v.c_str(),0,0,&bx,&by,&bw,&bh);
    oled.setCursor(max(0,(int)(64-(int)bw)/2),12);
    oled.print(v);
    oled.setCursor(0,24); oled.print(meterUnit);
    oled.setCursor(0,33); oled.print(lastReadingTime);
    oled.setCursor(0,42); oled.print(lastReadingDate);
    // Alarm
    if(alarmActive){oled.setCursor(36,42);oled.print("!ALM");}

  } else {
    // ── Stil 0: STANDARD ─────────────────────────────────────────────────────
    oled.setTextSize(1);
    String lbl = meterLabel; if(lbl.length()>9) lbl=lbl.substring(0,9);
    oled.setCursor(0,0); oled.print(lbl);
    oled.drawLine(0,9,63,9,SSD1306_WHITE);
    int vx = max(0,(64-(int)v.length()*6)/2);
    oled.setCursor(vx,11); oled.print(v);
    String unit = meterUnit; if(unit.length()>4) unit=unit.substring(0,4);
    oled.setCursor(0,21); oled.print(unit);
    if(alarmActive){oled.setCursor(36,21);oled.print("!ALM");}
    oled.setCursor(0,30); oled.print(lastReadingTime);
    oled.setCursor(0,39); oled.print(lastReadingDate);
    if(fetchOk) oled.fillCircle(61,42,3,SSD1306_WHITE);
    else        oled.drawCircle(61,42,3,SSD1306_WHITE);
  }

  oled.display();
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
//  Der ESP32 schreibt sich via simple-api in ioBroker ein:
//    metermaster.0.nodes.{MAC}.ip        → IP-Adresse
//    metermaster.0.nodes.{MAC}.name      → Gerätename
//    metermaster.0.nodes.{MAC}.version   → Firmware-Version
//    metermaster.0.nodes.{MAC}.lastSeen  → Unix-Timestamp (ms)
//    metermaster.0.nodes.{MAC}.config    → wird vom Adapter beschrieben
// ─────────────────────────────────────────────────────────────────────────────
bool iobSet(const String& statePath, const String& value) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  String url = "http://" + iobHost + ":" + String(iobPort)
             + "/set/" + statePath + "?value=" + value;
  http.begin(url); http.setTimeout(3000);
  int code = http.GET();
  http.end();
  return (code == 200);
}

String iobGet(const String& statePath) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  String url = "http://" + iobHost + ":" + String(iobPort) + "/get/" + statePath;
  http.begin(url); http.setTimeout(3000);
  int code = http.GET();
  String val = "";
  if (code == 200) {
    StaticJsonDocument<64> f; f["val"] = true;
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, http.getString(), DeserializationOption::Filter(f))) {
      JsonVariant s;
      if(doc.is<JsonArray>()) s=doc[0]; else s=doc.as<JsonVariant>();
      val = s["val"].as<String>();
    }
  }
  http.end();
  return val;
}

void registerNode() {
  if (iobHost.isEmpty() || iobPort == 0) return;
  String base = "metermaster.0.nodes." + nodeMac + ".";
  String ts = String(millis());  // Uptime als Proxy (kein NTP-abhängig)
  // echten Timestamp wenn NTP sync
  if (ntpSynced) {
    struct tm ti; if (getLocalTime(&ti)) {
      time_t t = mktime(&ti);
      char tsbuf[24]; snprintf(tsbuf, sizeof(tsbuf), "%lld", (long long)t * 1000LL); ts = String(tsbuf);
    }
  }
  iobSet(base+"ip",       WiFi.localIP().toString());
  iobSet(base+"name",     nodeName);
  iobSet(base+"version",  FW_VERSION);
  iobSet(base+"lastSeen", ts);
  addLog("Node registriert: " + nodeMac);
  Serial.println("Node registered: " + nodeMac);
}

// Liest config-State vom ioBroker – Adapter schreibt JSON rein
// Format: {"sid":"metermaster.0.X","label":"Strom","unit":"kWh"}
void pollConfig() {
  String base = "metermaster.0.nodes." + nodeMac + ".";
  String cfg = iobGet(base + "config");
  if (cfg.isEmpty() || cfg == "null" || cfg == "") return;

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, cfg)) return;

  bool changed = false;
  if (doc.containsKey("sid") && doc["sid"].as<String>() != stateId) {
    stateId    = doc["sid"].as<String>();
    meterLabel = doc["label"] | meterLabel;
    meterUnit  = doc["unit"]  | meterUnit;
    changed    = true;
  }
  if (doc.containsKey("carouselSec")) {
    int s = doc["carouselSec"].as<int>();
    if (s > 0) carouselSec = s;
    changed = true;
  }
  if (doc.containsKey("carouselActive")) {
    carouselActive = doc["carouselActive"].as<bool>();
    changed = true;
  }
  // Neuen Carousel-Eintrag vom Adapter empfangen
  if (doc.containsKey("carousel") && doc["carousel"].is<JsonArray>()) {
    JsonArray arr = doc["carousel"].as<JsonArray>();
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
  if (changed) {
    saveSettings();
    saveCarousel();
    lastFetch = 0;  // sofort neu laden
    addLog("Config vom ioBroker übernommen");
  Serial.println("Config vom ioBroker übernommen");
    // Config-State zurücksetzen (quittieren)
    iobSet(base + "configAck", String(millis()));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────────────────────────────────────
void loadSettings() {
  prefs.begin("mm", true);
  iobHost        = prefs.getString("host",  "192.168.178.113");
  iobPort        = prefs.getInt   ("port",  8087);
  stateId        = prefs.getString("sid",   "metermaster.0.MeinHaus.Westerheim.Warmwasser.readings.latest");
  meterLabel     = prefs.getString("lbl",   "Warmwasser");
  meterUnit      = prefs.getString("unit",  "m³");
  fetchSec       = prefs.getInt   ("intv",  60);
  alarmEnabled   = prefs.getBool  ("alEn",  false);
  alarmThreshold = prefs.getFloat ("alThr", 100.0);
  alarmAbove     = prefs.getBool  ("alAb",  true);
  prefs.end();
}
void saveSettings() {
  prefs.begin("mm", false);
  prefs.putString("host", iobHost);
  prefs.putInt   ("port", iobPort);
  prefs.putString("sid",  stateId);
  prefs.putString("lbl",  meterLabel);
  prefs.putString("unit", meterUnit);
  prefs.putInt   ("intv", fetchSec);
  prefs.putBool  ("alEn", alarmEnabled);
  prefs.putFloat ("alThr",alarmThreshold);
  prefs.putBool  ("alAb", alarmAbove);
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
//  HTML  (PROGMEM-Segmente)
// ─────────────────────────────────────────────────────────────────────────────

const char H_HEAD[] PROGMEM = R"RAW(<!DOCTYPE html>
<html lang="de"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MeterMaster Node</title><style>
:root{--bg:#0d0b14;--card:#18152a;--brd:#2d2545;--pri:#8b5cf6;--ph:#7c3aed;
      --p2:#a78bfa;--ok:#22c55e;--err:#ef4444;--warn:#f59e0b;
      --txt:#f1f5f9;--mut:#8b89a0;--r:10px}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--txt);font-family:'Segoe UI',system-ui,sans-serif;min-height:100vh}
.bar{background:var(--card);border-bottom:1px solid var(--brd);padding:12px 16px;
     display:flex;align-items:center;gap:11px}
.bar .ico{width:32px;height:32px;background:linear-gradient(135deg,#7c3aed,#a78bfa);
          border-radius:8px;display:flex;align-items:center;justify-content:center;font-size:17px}
.bar h1{font-size:.95rem;font-weight:700;background:linear-gradient(90deg,#a78bfa,#c4b5fd);
        -webkit-background-clip:text;-webkit-text-fill-color:transparent}
.bar .ip{font-size:.7rem;color:var(--mut);margin-left:auto}
.tabs{display:flex;background:var(--card);border-bottom:1px solid var(--brd);
      padding:0 12px;overflow-x:auto;-webkit-overflow-scrolling:touch}
.tab{padding:10px 12px;cursor:pointer;border-bottom:2px solid transparent;
     font-size:.78rem;color:var(--mut);user-select:none;white-space:nowrap;flex-shrink:0;transition:all .15s}
.tab.act{color:var(--p2);border-bottom-color:var(--pri)}
.tab:hover:not(.act){color:var(--txt)}
.pg{display:none;padding:18px 14px;max-width:560px;margin:0 auto}
.pg.act{display:block}
.card{background:var(--card);border:1px solid var(--brd);border-radius:var(--r);padding:16px;margin-bottom:12px}
.card.acc{border-color:#3d2d6a;background:linear-gradient(135deg,#18152a 60%,#1e1535)}
.h2{font-size:.73rem;color:var(--mut);text-transform:uppercase;letter-spacing:.07em;
    margin-bottom:12px;display:flex;align-items:center;gap:6px}
.h2 i{display:inline-flex;width:18px;height:18px;background:rgba(139,92,246,.18);
       border-radius:4px;align-items:center;justify-content:center;font-size:11px;font-style:normal}
.vdisp{text-align:center;padding:12px 0 4px}
.vnum{font-size:2.4rem;font-weight:800;background:linear-gradient(135deg,#a78bfa,#c4b5fd);
      -webkit-background-clip:text;-webkit-text-fill-color:transparent}
.vu{font-size:.9rem;color:var(--mut);margin-left:3px}
.vlbl{font-size:.8rem;color:var(--mut);margin-top:2px}
.vts{font-size:.72rem;color:var(--p2);margin-top:4px;opacity:.85}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px}
.cell{background:rgba(0,0,0,.25);border:1px solid var(--brd);border-radius:7px;padding:8px 11px}
.cell .k{font-size:.68rem;color:var(--mut);margin-bottom:2px}
.cell .v{font-size:.85rem;font-weight:600}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:4px;vertical-align:middle}
.dok{background:var(--ok)}.derr{background:var(--err)}.dwarn{background:var(--warn)}
/* Inputs */
label{display:block;font-size:.78rem;color:var(--mut);margin:11px 0 4px}
label:first-child{margin-top:0}
input,select{width:100%;padding:7px 10px;background:rgba(0,0,0,.3);border:1px solid var(--brd);
      border-radius:7px;color:var(--txt);font-size:.85rem;outline:none;transition:border .15s;
      -webkit-appearance:none;appearance:none}
input:focus,select:focus{border-color:var(--pri)}
select{background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='10' height='6' viewBox='0 0 10 6'%3E%3Cpath fill='%238b89a0' d='M0 0l5 6 5-6z'/%3E%3C/svg%3E");
       background-repeat:no-repeat;background-position:right 10px center;padding-right:28px}
select option{background:#18152a}
.r2{display:grid;grid-template-columns:1fr 1fr;gap:7px}
.r3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:7px}
/* Buttons */
.btn{display:block;width:100%;padding:9px;background:linear-gradient(135deg,#7c3aed,#8b5cf6);
     color:#fff;border:none;border-radius:7px;cursor:pointer;font-size:.85rem;font-weight:600;
     margin-top:11px;transition:opacity .15s;text-align:center}
.btn:hover{opacity:.87}
.btn.sec{background:transparent;border:1px solid var(--brd);color:var(--mut);margin-top:11px}
.btn.sec:hover{border-color:var(--pri);color:var(--p2)}
.btn.sm{padding:6px 12px;margin-top:0;width:auto;display:inline-block;font-size:.78rem}
.btn.red{background:linear-gradient(135deg,#991b1b,#ef4444)}
.btn.grn{background:linear-gradient(135deg,#166534,#22c55e)}
.row-btn{display:flex;gap:7px;align-items:flex-end}
.row-btn select,.row-btn input{flex:1;min-width:0}
/* Alerts */
.al{padding:8px 12px;border-radius:7px;font-size:.8rem;margin:8px 0;display:none}
.al.show{display:block}
.al-ok{background:#14532d;color:#86efac;border:1px solid #166534}
.al-err{background:#450a0a;color:#fca5a5;border:1px solid #7f1d1d}
.al-inf{background:#1e1854;color:#a5b4fc;border:1px solid #3730a3}
.al-warn{background:#451a03;color:#fcd34d;border:1px solid #78350f}
/* WLAN */
.sigbar{display:flex;align-items:flex-end;gap:2px;height:16px}
.sigbar span{width:6px;background:var(--brd);border-radius:2px}
.sigbar span.on{background:var(--pri)}
.sigbar span:nth-child(1){height:25%}.sigbar span:nth-child(2){height:50%}
.sigbar span:nth-child(3){height:75%}.sigbar span:nth-child(4){height:100%}
.ring-wrap{position:relative;width:58px;height:58px;flex-shrink:0}
.ring-wrap svg{transform:rotate(-90deg)}
.rbg{fill:none;stroke:var(--brd);stroke-width:6}
.rfg{fill:none;stroke:var(--pri);stroke-width:6;stroke-linecap:round;transition:stroke-dashoffset .4s}
.rpct{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);
      font-size:.7rem;font-weight:700;color:var(--p2)}
.irow{display:flex;justify-content:space-between;align-items:center;
      padding:7px 0;border-bottom:1px solid var(--brd);font-size:.84rem}
.irow:last-child{border-bottom:none}
.irow .ik{color:var(--mut);font-size:.77rem}
.irow .iv{font-weight:600;text-align:right}
/* OTA */
.drop{border:2px dashed var(--brd);border-radius:var(--r);padding:30px 18px;
      text-align:center;color:var(--mut);cursor:pointer;transition:all .15s}
.drop:hover,.drop.ov{border-color:var(--pri);background:rgba(139,92,246,.05)}
.drop .ei{font-size:1.8rem;margin-bottom:6px}
.drop .fn{margin-top:8px;font-size:.75rem;color:var(--p2)}
.pb{height:6px;background:var(--brd);border-radius:3px;overflow:hidden;margin-top:10px;display:none}
.pb.show{display:block}
.pbi{height:100%;width:0%;background:linear-gradient(90deg,#7c3aed,#a78bfa);border-radius:3px;transition:width .15s}
/* BLE */
.ble-item{background:rgba(0,0,0,.2);border:1px solid var(--brd);border-radius:7px;
           padding:8px 11px;margin-bottom:6px;font-size:.8rem}
.ble-item .bn{font-weight:600;color:var(--p2)}
.ble-item .ba{color:var(--mut);font-size:.73rem;margin-top:2px;word-break:break-all}
.ble-item .br{color:var(--mut);font-size:.73rem}
/* Alarm */
.thr-row{display:flex;align-items:center;gap:8px;margin-top:4px}
.thr-row input{flex:1}
.alarm-badge{display:inline-flex;align-items:center;gap:5px;padding:4px 10px;
             border-radius:20px;font-size:.75rem;font-weight:600}
.alarm-off{background:rgba(0,0,0,.2);color:var(--mut);border:1px solid var(--brd)}
.alarm-on{background:rgba(239,68,68,.15);color:#fca5a5;border:1px solid #7f1d1d}
.pulse{animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
.style-btn{background:rgba(0,0,0,.25);border:2px solid var(--brd);border-radius:8px;padding:8px 6px;text-align:center;cursor:pointer;font-size:.75rem;font-weight:600;transition:all .15s}
.style-btn:hover{border-color:var(--pri)}
.style-btn.act{border-color:var(--pri);background:rgba(139,92,246,.15);color:var(--p2)}
.sico{font-size:1.2rem;margin-bottom:3px}
.sdesc{font-size:.65rem;color:var(--mut);font-weight:400;margin-top:2px}
</style></head><body>
<div class="bar">
  <div class="ico">⚡</div><h1>MeterMaster Node</h1>
  <span class="ip" id="topIp"></span>
</div>
<div class="tabs">
  <div class="tab act" onclick="tab('db')">Dashboard</div>
  <div class="tab"     onclick="tab('cfg')">Einstellungen</div>
  <div class="tab"     onclick="tab('wl')">WLAN</div>
  <div class="tab"     onclick="tab('al')">Alarm</div>
  <div class="tab"     onclick="tab('bt')">Bluetooth</div>
  <div class="tab"     onclick="tab('car')">Carousel</div>
  <div class="tab"     onclick="tab('dbg')">Debug</div>
  <div class="tab"     onclick="tab('ota')">OTA</div>
  <div class="tab"     onclick="tab('info')">Info</div>
</div>
)RAW";

const char H_DB[] PROGMEM = R"RAW(
<div id="db" class="pg act">
  <div class="card acc">
    <div class="h2"><i>📊</i>Aktueller Zählerwert</div>
    <div class="vdisp">
      <div><span class="vnum" id="dVal">–</span><span class="vu" id="dUnit"></span></div>
      <div class="vlbl" id="dLbl"></div>
      <div class="vts" id="dTs">Ablesung: –</div>
    </div>
    <div class="g2">
      <div class="cell"><div class="k">Status</div>
        <div class="v"><span class="dot derr" id="dDot"></span><span id="dSt">...</span></div></div>
      <div class="cell"><div class="k">Letztes Update</div><div class="v" id="dAge">–</div></div>
    </div>
    <div id="alarmBadge" class="alarm-badge alarm-off" style="margin-top:10px">
      <span id="alarmDot">🔔</span><span id="alarmTxt">Kein Alarm</span>
    </div>
  </div>

  <div class="card">
    <div class="h2"><i>💡</i>Rote LED</div>
    <div style="display:flex;align-items:center;gap:12px">
      <button id="ledBtn" class="btn" style="margin-top:0;flex:1" onclick="toggleLed()">LED einschalten</button>
      <div style="text-align:center;min-width:44px">
        <div id="ledDot" style="width:16px;height:16px;border-radius:50%;background:#374151;
             border:2px solid #4b5563;margin:0 auto;transition:all .3s"></div>
        <div id="ledLbl" style="font-size:.68rem;color:var(--mut);margin-top:3px">Aus</div>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="h2"><i>🖥</i>OLED Zähler &amp; Stil</div>
    <label>Zähler auswählen</label>
    <select id="oSel" onchange="oSelChange()">
      <option value="">– Zähler in Einstellungen laden –</option>
    </select>
    <label style="margin-top:10px">Bezeichnung (OLED)</label>
    <div class="r2">
      <input id="oLbl" placeholder="Bezeichnung">
      <input id="oUnt" placeholder="Einheit">
    </div>
    <label>Display-Stil</label>
    <div class="g2" id="styleGrid" style="gap:6px">
      <div class="style-btn act" data-s="0" onclick="setStyle(0)">
        <div class="sico">☰</div><div>Standard</div><div class="sdesc">Label+Wert+Zeit+Datum</div>
      </div>
      <div class="style-btn" data-s="1" onclick="setStyle(1)">
        <div class="sico">🔢</div><div>Groß</div><div class="sdesc">Großer Wert, kein Datum</div>
      </div>
      <div class="style-btn" data-s="2" onclick="setStyle(2)">
        <div class="sico">◻</div><div>Minimal</div><div class="sdesc">Nur Wert + Einheit</div>
      </div>
      <div class="style-btn" data-s="3" onclick="setStyle(3)">
        <div class="sico">◼</div><div>Invertiert</div><div class="sdesc">Weiß auf Schwarz</div>
      </div>
    </div>
    <button class="btn" onclick="saveOled()">Auf OLED anwenden &amp; speichern</button>
    <div id="alOled" class="al"></div>
  </div>
</div>
)RAW";

const char H_CFG[] PROGMEM = R"RAW(
<div id="cfg" class="pg">
  <div id="alCfg" class="al"></div>
  <div class="card">
    <div class="h2"><i>🔌</i>ioBroker Simple-API</div>
    <label>Host / IP</label>
    <input id="cHost" placeholder="192.168.178.113">
    <div class="r2">
      <div><label>Port</label><input id="cPort" type="number"></div>
      <div><label>Intervall (s)</label><input id="cInt" type="number"></div>
    </div>
    <div class="r2" style="margin-top:11px">
      <button class="btn sec" style="margin-top:0" onclick="testConn()">🔗 Verbindung testen</button>
      <button class="btn sm" style="margin-top:0;width:100%" onclick="discover()">🔍 Zähler laden</button>
    </div>
    <div id="alDisc" class="al"></div>
  </div>
  <div class="card">
    <div class="h2"><i>🔢</i>Verfügbare Zähler</div>
    <div class="row-btn">
      <select id="discSel" onchange="discSelect()">
        <option value="">– erst "Zähler laden" klicken –</option>
      </select>
    </div>
    <div id="discMeta" style="font-size:.73rem;color:var(--mut);margin-top:5px;min-height:14px"></div>
    <label style="margin-top:12px">State-ID (manuell)</label>
    <input id="cSid">
    <div class="r2">
      <div><label>Bezeichnung</label><input id="cLbl"></div>
      <div><label>Einheit</label><input id="cUnt"></div>
    </div>
    <button class="btn" onclick="saveCfg()">💾 Einstellungen speichern</button>
  </div>
  <div class="card">
    <div class="h2"><i>🔗</i>MeterMaster ioBroker Adapter</div>
    <div class="irow"><span class="ik">Adapter Version</span><span class="iv" id="adapterVerVal" style="font-size:.78rem;color:var(--p2)">Prüfe…</span></div>
    <div class="irow"><span class="ik">GitHub</span><a href="https://github.com/MPunktBPunkt/iobroker.metermaster" target="_blank" style="font-size:.78rem;color:var(--p2);text-decoration:none">MPunktBPunkt/iobroker.metermaster ↗</a></div>
    <div class="irow"><span class="ik">npm</span><a href="https://www.npmjs.com/package/iobroker.metermaster" target="_blank" style="font-size:.78rem;color:var(--p2);text-decoration:none">npmjs.com/package/iobroker.metermaster ↗</a></div>
  </div>
</div>
)RAW";

const char H_WL[] PROGMEM = R"RAW(
<div id="wl" class="pg">
  <div class="card acc">
    <div class="h2"><i>📶</i>WLAN-Verbindung</div>
    <div style="display:flex;align-items:center;gap:14px;margin-bottom:12px">
      <div class="sigbar"><span id="sb1"></span><span id="sb2"></span><span id="sb3"></span><span id="sb4"></span></div>
      <div><div style="font-weight:700" id="wSSID">–</div>
           <div style="font-size:.75rem;color:var(--mut)" id="wRSSI">–</div></div>
      <button class="btn sm sec" style="margin-left:auto" onclick="loadSys()">🔄</button>
    </div>
    <div id="alWl" class="al"></div>
    <div class="irow"><span class="ik">IP-Adresse</span><span class="iv" id="wIP">–</span></div>
    <div class="irow"><span class="ik">MAC</span><span class="iv" id="wMAC">–</span></div>
    <div class="irow"><span class="ik">Kanal</span><span class="iv" id="wCH">–</span></div>
    <div class="irow"><span class="ik">NTP-Zeit</span><span class="iv" id="wNTP">–</span></div>
  </div>
  <div class="card">
    <div class="h2"><i>💾</i>Speicher</div>
    <div style="display:flex;gap:14px;align-items:center">
      <div class="ring-wrap">
        <svg width="58" height="58" viewBox="0 0 58 58">
          <circle class="rbg" cx="29" cy="29" r="23"/>
          <circle class="rfg" id="hRing" cx="29" cy="29" r="23" stroke-dasharray="144.5" stroke-dashoffset="144.5"/>
        </svg>
        <div class="rpct" id="hPct">–</div>
      </div>
      <div style="flex:1">
        <div class="irow" style="padding:4px 0"><span class="ik">Heap frei</span><span class="iv" id="hFree">–</span></div>
        <div class="irow" style="padding:4px 0"><span class="ik">Gesamt</span><span class="iv" id="hTotal">–</span></div>
      </div>
    </div>
  </div>
  <div class="card">
    <div class="h2"><i>📦</i>Sketch / System</div>
    <div class="irow"><span class="ik">Sketch belegt</span><span class="iv" id="sUsed">–</span></div>
    <div class="irow"><span class="ik">Sketch frei</span><span class="iv" id="sFree">–</span></div>
    <div class="irow"><span class="ik">Chip</span><span class="iv" id="sChip">–</span></div>
    <div class="irow"><span class="ik">CPU-Takt</span><span class="iv" id="sCPU">–</span></div>
    <div class="irow"><span class="ik">Uptime</span><span class="iv" id="sUp">–</span></div>
  </div>
</div>
)RAW";

const char H_AL[] PROGMEM = R"RAW(
<div id="al" class="pg">
  <div id="alAl" class="al"></div>
  <div class="card acc">
    <div class="h2"><i>🔔</i>Alarm-Status</div>
    <div id="alStatusCard" class="alarm-badge alarm-off" style="margin-bottom:12px;font-size:.82rem">
      <span id="alStatDot">🔔</span><span id="alStatTxt">Alarm deaktiviert</span>
    </div>
    <div class="g2">
      <div class="cell"><div class="k">Aktueller Wert</div><div class="v" id="alCurVal">–</div></div>
      <div class="cell"><div class="k">Schwellwert</div><div class="v" id="alThrDisp">–</div></div>
    </div>
  </div>
  <div class="card">
    <div class="h2"><i>⚙️</i>Alarm konfigurieren</div>
    <label>Schwellwert</label>
    <input id="alThr" type="number" step="0.1" placeholder="100.0">
    <label>Bedingung</label>
    <select id="alMode">
      <option value="above">Alarm wenn Wert ÜBER Schwellwert</option>
      <option value="below">Alarm wenn Wert UNTER Schwellwert</option>
    </select>
    <label style="margin-top:12px">Alarm-Aktionen</label>
    <div class="r2">
      <div class="cell" style="display:flex;align-items:center;gap:8px;cursor:pointer" onclick="togCb('alLed')">
        <input type="checkbox" id="alLed" style="width:auto;cursor:pointer">
        <span style="font-size:.82rem">LED blinken</span>
      </div>
      <div class="cell" style="display:flex;align-items:center;gap:8px;cursor:pointer" onclick="togCb('alEn')">
        <input type="checkbox" id="alEn" style="width:auto;cursor:pointer">
        <span style="font-size:.82rem">Alarm aktiv</span>
      </div>
    </div>
    <button class="btn" onclick="saveAlarm()">🔔 Alarm speichern</button>
  </div>
  <div class="card">
    <div class="h2"><i>ℹ️</i>Hinweise</div>
    <p style="font-size:.8rem;color:var(--mut);line-height:1.5">
      Bei aktivem Alarm blinkt die rote LED und das OLED zeigt <strong>!ALM</strong>.<br>
      Der Alarm wird nach jedem Datenabruf geprüft (Intervall: Einstellungen).
    </p>
  </div>
</div>
)RAW";

const char H_BT[] PROGMEM = R"RAW(
<div id="bt" class="pg">
  <div class="card acc">
    <div class="h2"><i>🔵</i>Bluetooth / BLE</div>
    <div style="text-align:center;padding:20px 0">
      <div style="font-size:2.5rem;margin-bottom:10px">🔵</div>
      <div style="font-weight:700;margin-bottom:8px">In Vorbereitung</div>
      <p style="font-size:.8rem;color:var(--mut);line-height:1.6;max-width:300px;margin:0 auto">
        BLE benötigt ca. 500 KB zusätzlichen Flash-Speicher.<br>
        Für BLE-Support in Arduino IDE unter<br>
        <strong>Tools → Partition Scheme</strong> wählen:<br><br>
        <code style="background:rgba(0,0,0,.3);padding:3px 7px;border-radius:4px">Huge APP (3MB No OTA)</code><br><br>
        Danach kann BLE-Scan aktiviert werden.
      </p>
    </div>
  </div>
  <div class="card">
    <div class="h2"><i>🔮</i>Geplante BLE-Features</div>
    <p style="font-size:.8rem;color:var(--mut);line-height:1.7">
      • BLE-Scan: Geräte in der Nähe anzeigen<br>
      • Xiaomi LYWSD03MMC Thermometer direkt auslesen<br>
      • Temperatur/Luftfeuchtigkeit auf OLED anzeigen<br>
      • BLE → ioBroker Gateway-Funktion<br>
      • iBeacon-Erkennung für Präsenz-Automation
    </p>
  </div>
</div>
)RAW";

const char H_INFO[] PROGMEM = R"RAW(
<div id="info" class="pg">
  <div class="card acc" style="text-align:center;padding:24px 16px">
    <div style="font-size:2.2rem;margin-bottom:8px">⚡</div>
    <div style="font-weight:800;font-size:1.1rem;background:linear-gradient(90deg,#a78bfa,#c4b5fd);
         -webkit-background-clip:text;-webkit-text-fill-color:transparent">MeterMaster Node</div>
    <div style="font-size:.78rem;color:var(--mut);margin-top:4px" id="infoVer">v–</div>
    <div style="font-size:.75rem;color:var(--mut);margin-top:2px">ESP32 D1 Mini · OLED 64×48</div>
  </div>
  <div class="card">
    <div class="h2"><i>📝</i>Changelog</div>
    <div class="irow"><span class="ik">v0.2.0</span><span class="iv" style="font-size:.75rem;text-align:right">Carousel, Node-Reg.,<br>Debug-Tab, Bugfixes</span></div>
    <div class="irow"><span class="ik">v0.1.4</span><span class="iv" style="font-size:.75rem;text-align:right">Info-Tab, GitHub OTA,<br>OLED-Stile, Adapter-Version</span></div>
    <div class="irow"><span class="ik">v0.1.3</span><span class="iv" style="font-size:.75rem;text-align:right">Alarm-Tab, Bluetooth-Tab,<br>Discover im Einstellungen-Tab</span></div>
    <div class="irow"><span class="ik">v0.1.2</span><span class="iv" style="font-size:.75rem;text-align:right">NTP-Zeit, WLAN-Tab,<br>LED-Steuerung, Lila Theme</span></div>
    <div class="irow"><span class="ik">v0.1.1</span><span class="iv" style="font-size:.75rem;text-align:right">ioBroker Discover-Dropdown,<br>OTA Update, Web-Interface</span></div>
    <div class="irow"><span class="ik">v0.1.0</span><span class="iv" style="font-size:.75rem;text-align:right">Erstveröffentlichung</span></div>
  </div>
  <div class="card">
    <div class="h2"><i>🔗</i>GitHub Repositories</div>
    <div class="irow">
      <span class="ik">ESP32 Node</span>
      <a id="ghEspLink" href="#" target="_blank"
         style="font-size:.78rem;color:var(--p2);text-decoration:none">
        MPunktBPunkt/esp32.MeterMaster ↗
      </a>
    </div>
    <div class="irow">
      <span class="ik">ioBroker Adapter</span>
      <a href="https://github.com/MPunktBPunkt/iobroker.metermaster" target="_blank"
         style="font-size:.78rem;color:var(--p2);text-decoration:none">
        MPunktBPunkt/iobroker.metermaster ↗
      </a>
    </div>
    <div class="irow">
      <span class="ik">MeterMaster App</span>
      <a href="https://github.com/MPunktBPunkt" target="_blank"
         style="font-size:.78rem;color:var(--p2);text-decoration:none">
        github.com/MPunktBPunkt ↗
      </a>
    </div>
  </div>
  <div class="card">
    <div class="h2"><i>⚖️</i>Lizenz &amp; Kontakt</div>
    <p style="font-size:.8rem;color:var(--mut);line-height:1.6">
      Open Source – MIT Lizenz<br>
      Entwickelt als Companion-Hardware für die MeterMaster App &amp; den ioBroker-Adapter.<br>
      Issues &amp; Feature-Requests gerne auf GitHub!
    </p>
  </div>
</div>
)RAW";

const char H_CAR[] PROGMEM = R"RAW(
<div id="car" class="pg">
  <div id="alCar" class="al"></div>

  <div class="card acc">
    <div class="h2"><i>🎠</i>Carousel-Status</div>
    <div class="g2">
      <div class="cell"><div class="k">Carousel aktiv</div>
        <div class="v" id="carActive">–</div></div>
      <div class="cell"><div class="k">Aktuell angezeigt</div>
        <div class="v" id="carCurrent">–</div></div>
    </div>
    <div style="display:flex;gap:7px;margin-top:12px">
      <button class="btn sec" style="margin-top:0;flex:1" onclick="carPrev()">⏮ Zurück</button>
      <button class="btn" style="margin-top:0;flex:1" onclick="carNext()">Weiter ⏭</button>
    </div>
  </div>

  <div class="card">
    <div class="h2"><i>⚙️</i>Carousel konfigurieren</div>
    <div class="r2">
      <div class="cell" style="display:flex;align-items:center;gap:8px;cursor:pointer" onclick="togCb('carOn')">
        <input type="checkbox" id="carOn" style="width:auto;cursor:pointer">
        <span style="font-size:.82rem">Carousel ein</span>
      </div>
      <div>
        <label style="margin-top:0">Intervall (s)</label>
        <input id="carSec" type="number" min="3" max="300" value="10">
      </div>
    </div>
    <div style="display:flex;gap:7px;margin-top:12px">
      <button class="btn" style="margin-top:0;flex:1" onclick="carSave()">💾 Speichern</button>
      <button class="btn sec" style="margin-top:0" onclick="loadCarouselUI()">🔄</button>
    </div>
  </div>

  <div class="card">
    <div class="h2"><i>📋</i>Zähler-Liste
      <span style="margin-left:auto;font-size:.7rem;color:var(--mut)" id="carCount"></span>
    </div>
    <div id="carList" style="margin-bottom:10px">
      <div style="color:var(--mut);font-size:.8rem">Keine Zähler konfiguriert.</div>
    </div>
    <div style="background:rgba(0,0,0,.2);border:1px solid var(--brd);border-radius:8px;padding:12px">
      <div style="font-size:.75rem;color:var(--p2);font-weight:600;margin-bottom:8px">+ Zähler hinzufügen</div>
      <input id="carAddSid"   placeholder="State-ID (metermaster.0...)" style="margin-bottom:6px">
      <div class="r2">
        <input id="carAddLbl" placeholder="Bezeichnung">
        <input id="carAddUnt" placeholder="Einheit">
      </div>
      <div style="display:flex;gap:7px;margin-top:8px">
        <button class="btn sm sec" onclick="carFromDiscover()">📋 Aus Discover</button>
        <button class="btn sm" onclick="carAdd()">+ Hinzufügen</button>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="h2"><i>📡</i>Node-Registrierung</div>
    <label>Gerätename (in ioBroker sichtbar)</label>
    <div style="display:flex;gap:7px">
      <input id="nodeNameIn" placeholder="MeterMaster Node" style="flex:1">
      <button class="btn sm" onclick="saveNodeName()">💾</button>
    </div>
    <div class="irow" style="margin-top:10px"><span class="ik">MAC-Adresse</span><span class="iv" id="carMac">–</span></div>
    <div class="irow"><span class="ik">ioBroker State-Pfad</span><span class="iv" id="carStatePath" style="font-size:.72rem;text-align:right">–</span></div>
    <p style="font-size:.77rem;color:var(--mut);margin-top:10px;line-height:1.5">
      Der ESP32 schreibt sich automatisch in ioBroker ein.<br>
      Der MeterMaster-Adapter liest <code style="background:rgba(0,0,0,.3);padding:1px 4px;border-radius:3px">metermaster.0.nodes.*</code>
      und zeigt alle Nodes in der Admin-UI.
    </p>
  </div>
</div>
)RAW";

const char H_OTA[] PROGMEM = R"RAW(
<div id="ota" class="pg">
  <div class="card acc">
    <div class="h2"><i>🔍</i>Auf Updates prüfen</div>
    <div class="g2">
      <div class="cell"><div class="k">Installiert</div><div class="v" id="otaFwCur">–</div></div>
      <div class="cell"><div class="k">GitHub (aktuell)</div><div class="v" id="otaFwNew">–</div></div>
    </div>
    <div id="alOtaCheck" class="al"></div>
    <div style="display:flex;gap:7px;margin-top:10px">
      <button class="btn sec" style="margin-top:0;flex:1" onclick="checkUpdate()">🔍 Prüfen</button>
      <a id="otaDlBtn" class="btn" style="margin-top:0;flex:1;text-decoration:none;display:none;text-align:center"
         href="#" target="_blank">⬇ Download .bin</a>
    </div>
  </div>
  <div class="card">
    <div class="h2"><i>📦</i>Manuelles Update</div>
    <div id="alOta" class="al"></div>
    <div class="drop" id="dropZ" onclick="document.getElementById('fIn').click()"
         ondragover="evD(event,true)" ondragleave="evD(event,false)" ondrop="drpD(event)">
      <div class="ei">📦</div>
      <div><strong>Firmware .bin hier ablegen</strong></div>
      <div style="font-size:.75rem;margin-top:4px">oder klicken zum Auswählen</div>
      <div class="fn" id="fName"></div>
    </div>
    <input type="file" id="fIn" accept=".bin" style="display:none"
           onchange="if(this.files[0])upload(this.files[0])">
    <div class="pb" id="pb"><div class="pbi" id="pi"></div></div>
    <div class="g2" style="margin-top:12px">
      <div class="cell"><div class="k">Gerät</div><div class="v">ESP32 D1 Mini</div></div>
      <div class="cell"><div class="k">IP</div><div class="v" id="otaIp">–</div></div>
    </div>
    <p style="font-size:.75rem;color:var(--mut);margin-top:10px">
      .bin erzeugen: Arduino IDE → <em>Sketch → Exportiere kompilierte Binärdatei</em>
    </p>
  </div>
</div>
)RAW";

const char H_DBG[] PROGMEM = R"RAW(
<div id="dbg" class="pg">
  <div id="alDbg" class="al"></div>
  <div class="card acc">
    <div class="h2"><i>🪲</i>Debug Log
      <button class="btn sm sec" style="margin:0 0 0 auto;padding:4px 10px" onclick="clearLog()">Leeren</button>
      <button class="btn sm" style="margin:0 0 0 6px;padding:4px 10px" onclick="loadLog()">🔄</button>
    </div>
    <div style="display:flex;gap:8px;margin-bottom:10px;flex-wrap:wrap">
      <label style="display:flex;align-items:center;gap:6px;font-size:.8rem;cursor:pointer">
        <input type="checkbox" id="autoRef" checked style="width:auto"> Auto-Refresh (3s)
      </label>
      <span style="font-size:.75rem;color:var(--mut)" id="logMeta"></span>
    </div>
    <div id="logBox" style="background:#0d0d14;border:1px solid var(--brd);border-radius:8px;
         padding:10px;font-family:monospace;font-size:.72rem;max-height:420px;overflow-y:auto;
         color:#c4b5fd;line-height:1.7"></div>
  </div>
  <div class="card">
    <div class="h2"><i>💾</i>Speicher</div>
    <div class="g2">
      <div class="cell"><div class="k">Heap frei</div><div class="v" id="dbgHeap">–</div></div>
      <div class="cell"><div class="k">Min. Heap</div><div class="v" id="dbgMinHeap">–</div></div>
    </div>
    <div id="dbgHeapBar" style="margin-top:10px;height:6px;border-radius:3px;background:var(--brd)">
      <div id="dbgHeapFill" style="height:6px;border-radius:3px;background:linear-gradient(90deg,#a78bfa,#7c3aed);transition:width .4s;width:0%"></div>
    </div>
  </div>
  <div class="card">
    <div class="h2"><i>⚙️</i>Debug-Aktionen</div>
    <div style="display:flex;gap:7px;flex-wrap:wrap">
      <button class="btn sm sec" onclick="dbgFetch()">🔄 Fetch auslösen</button>
      <button class="btn sm sec" onclick="dbgRegister()">📡 Node registrieren</button>
      <button class="btn sm sec" onclick="dbgRestart()">♻️ ESP32 neu starten</button>
    </div>
    <div id="alDbgAct" class="al" style="margin-top:8px"></div>
  </div>
</div>
)RAW";

const char H_JS[] PROGMEM = R"RAW(
<script>
const TABS=['db','cfg','wl','al','bt','car','dbg','ota','info'];
let discStates=[];

function tab(id){
  TABS.forEach((t,i)=>{
    document.getElementById(t).classList.toggle('act',t===id);
    document.querySelectorAll('.tab')[i].classList.toggle('act',t===id);
  });
  if(id==='wl') loadSys();
  if(id==='al') loadAlarmUI();
  if(id==='info') loadInfo();
  if(id==='dbg')  loadLog();
  if(id==='car')  loadCarouselUI();
}

function al(id,type,msg,ms=0){
  const e=document.getElementById(id);
  e.className='al show al-'+(type==='ok'?'ok':type==='err'?'err':type==='warn'?'warn':'inf');
  e.textContent=msg;
  if(ms>0) setTimeout(()=>e.className='al',ms);
}

// ── Dashboard ─────────────────────────────────────────────────────────────────
function setLedUi(on){
  const d=document.getElementById('ledDot'),l=document.getElementById('ledLbl'),b=document.getElementById('ledBtn');
  if(!d)return;
  if(on){d.style.background='#ef4444';d.style.border='2px solid #f87171';d.style.boxShadow='0 0 8px #ef4444';
    l.textContent='An';b.textContent='LED ausschalten';b.className='btn red';}
  else{d.style.background='#374151';d.style.border='2px solid #4b5563';d.style.boxShadow='none';
    l.textContent='Aus';b.textContent='LED einschalten';b.className='btn';}
}
function toggleLed(){
  fetch('/api/led?state=toggle').then(r=>r.json()).then(d=>setLedUi(d.ledOn));
}

function setAlarmBadge(active,txt,elemId){
  const e=document.getElementById(elemId);
  if(!e)return;
  e.className='alarm-badge '+(active?'alarm-on pulse':'alarm-off');
  e.innerHTML=(active?'🚨':'🔔')+' <span>'+txt+'</span>';
}

function refreshDash(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('dVal').textContent=parseFloat(d.value).toFixed(2);
    document.getElementById('dUnit').textContent=' '+d.unit;
    document.getElementById('dLbl').textContent=d.label;
    document.getElementById('dDot').className='dot '+(d.ok?'dok':'derr');
    document.getElementById('dSt').textContent=d.ok?'Verbunden':d.status;
    document.getElementById('dAge').textContent=d.lastFetch;
    document.getElementById('dTs').textContent='Ablesung: '+d.readingDate+' '+d.readingTime;
    document.getElementById('topIp').textContent='📶 '+d.ip;
    document.getElementById('otaIp').textContent=d.ip;
    setLedUi(d.ledOn);
    setAlarmBadge(d.alarmActive, d.alarmActive?'ALARM AKTIV':'Kein Alarm','alarmBadge');
    // Dropdown vorbelegen
    const sel=document.getElementById('oSel');
    if(sel.options.length<=1&&d.stateId){
      const o=new Option(d.label+' ('+d.stateId+')',JSON.stringify({id:d.stateId,label:d.label,unit:d.unit}));
      sel.add(o); sel.value=o.value;
    }
    document.getElementById('oLbl').value=d.label;
    document.getElementById('oUnt').value=d.unit;
    if(d.oledStyle!==undefined) initStyleBtns(d.oledStyle);
  }).catch(()=>{});
}
setInterval(refreshDash,15000);

// ── OLED Dropdown ─────────────────────────────────────────────────────────────
function oSelChange(){
  const v=document.getElementById('oSel').value;
  if(!v)return;
  try{const s=JSON.parse(v);document.getElementById('oLbl').value=s.label;document.getElementById('oUnt').value=s.unit;}catch(e){}
}
function saveOled(){
  const sid=document.getElementById('oSel').value;
  let stateId="";
  try{stateId=JSON.parse(sid).id;}catch(e){stateId="";}
  if(!stateId){al('alOled','err','Bitte erst einen Zähler auswählen.');return;}
  const b={stateId:stateId,label:document.getElementById('oLbl').value,unit:document.getElementById('oUnt').value,oledStyle:curStyle};
  fetch('/api/oled',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)})
    .then(r=>r.json()).then(d=>al('alOled',d.ok?"ok":"err",d.ok?'✓ OLED aktualisiert & gespeichert.':'✗ '+d.msg,3000));
}

// ── Einstellungen ─────────────────────────────────────────────────────────────
function loadCfg(){
  loadAdapterVersion();
  fetch('/api/settings').then(r=>r.json()).then(d=>{
    document.getElementById('cHost').value=d.iobHost;
    document.getElementById('cPort').value=d.iobPort;
    document.getElementById('cSid').value=d.stateId;
    document.getElementById('cLbl').value=d.label;
    document.getElementById('cUnt').value=d.unit;
    document.getElementById('cInt').value=d.interval;
  });
}
function saveCfg(){
  const b={iobHost:document.getElementById('cHost').value,
           iobPort:+document.getElementById('cPort').value,
           stateId:document.getElementById('cSid').value,
           label:document.getElementById('cLbl').value,
           unit:document.getElementById('cUnt').value,
           interval:+document.getElementById('cInt').value};
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)})
    .then(r=>r.json()).then(d=>{al('alCfg',d.ok?"ok":"err",d.ok?'✓ Gespeichert.':'✗ '+d.msg,3000);});
}
function testConn(){
  al('alCfg','inf','Verbinde…');
  const b={iobHost:document.getElementById('cHost').value,
           iobPort:+document.getElementById('cPort').value,
           stateId:document.getElementById('cSid').value};
  fetch('/api/test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)})
    .then(r=>r.json()).then(d=>al('alCfg',d.ok?"ok":"err",d.ok?'✓ OK – Wert: '+parseFloat(d.value).toFixed(3):'✗ '+d.msg,4000));
}

let discStatesCfg=[];
function discover(){
  al('alDisc','inf','Lade Zählerliste von ioBroker…');
  document.getElementById('discSel').innerHTML='<option>Lade…</option>';
  fetch('/api/discover').then(r=>r.json()).then(d=>{
    if(!d.ok){al('alDisc','err','✗ '+d.msg);return;}
    discStatesCfg=d.states;
    const sel=document.getElementById('discSel');
    sel.innerHTML='<option value="">– Zähler wählen –</option>';
    // Dashboard-Dropdown auch befüllen
    const oSel=document.getElementById('oSel');
    oSel.innerHTML='<option value="">– Zähler wählen –</option>';
    d.states.forEach((s,i)=>{
      const txt=s.label+' ('+parseFloat(s.val).toFixed(2)+(s.unit?' '+s.unit:"")+')';
      const val=JSON.stringify({id:s.id,label:s.label.split(' · ').slice(-2,-1)[0]||s.label,unit:s.unit});
      sel.add(new Option(txt,i));
      oSel.add(new Option(txt,val));
    });
    document.getElementById('discMeta').textContent=d.states.length+' Zähler gefunden.';
    al('alDisc','ok','✓ '+d.states.length+' Zähler geladen.',3000);
  }).catch(e=>al('alDisc','err','✗ '+e));
}
function discSelect(){
  const idx=document.getElementById('discSel').value;
  if(idx==="")return;
  const s=discStatesCfg[parseInt(idx)];
  document.getElementById('cSid').value=s.id;
  const parts=s.label.split(' · ');
  document.getElementById('cLbl').value=parts.length>=2?parts[parts.length-2]:s.label;
  document.getElementById('cUnt').value=s.unit;
  document.getElementById('discMeta').textContent='Wert: '+parseFloat(s.val).toFixed(3)+(s.unit?' '+s.unit:"");
}

// ── WLAN ──────────────────────────────────────────────────────────────────────
function fmtB(b){return b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' KB':(b/1048576).toFixed(1)+' MB';}
function fmtUp(s){const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;return(h?h+"h ":"")+m+'m '+sec+'s';}
function setSig(q){[25,50,75,100].forEach((l,i)=>document.getElementById('sb'+(i+1)).classList.toggle('on',q>=l));}
function loadSys(){
  fetch('/api/sysinfo').then(r=>r.json()).then(d=>{
    document.getElementById('wSSID').textContent=d.ssid;
    document.getElementById('wRSSI').textContent=d.rssi+' dBm ('+d.quality+'%)';
    document.getElementById('wIP').textContent=d.ip;
    document.getElementById('wMAC').textContent=d.mac;
    document.getElementById('wCH').textContent='Kanal '+d.channel;
    document.getElementById('wNTP').textContent=d.ntpSynced?d.nowDate+' '+d.nowTime+' ✓':'Nicht synchronisiert';
    setSig(d.quality);
    const used=Math.round(100-d.heapFree/d.heapTotal*100);
    document.getElementById('hRing').style.strokeDashoffset=144.5*(1-used/100);
    document.getElementById('hPct').textContent=used+'%';
    document.getElementById('hFree').textContent=fmtB(d.heapFree);
    document.getElementById('hTotal').textContent=fmtB(d.heapTotal);
    document.getElementById('sUsed').textContent=fmtB(d.sketchUsed);
    document.getElementById('sFree').textContent=fmtB(d.sketchFree);
    document.getElementById('sChip').textContent=d.chipModel+' @ '+d.cpuFreq+' MHz';
    document.getElementById('sCPU').textContent=d.cpuFreq+' MHz';
    document.getElementById('sUp').textContent=fmtUp(d.uptime);
  }).catch(e=>al('alWl','err','Fehler: '+e));
}

// ── Alarm ─────────────────────────────────────────────────────────────────────
function togCb(id){const c=document.getElementById(id);c.checked=!c.checked;}
function loadAlarmUI(){
  fetch('/api/settings').then(r=>r.json()).then(d=>{
    document.getElementById('alThr').value=d.alarmThreshold||100;
    document.getElementById('alMode').value=d.alarmAbove?'above':'below';
    document.getElementById('alEn').checked=d.alarmEnabled||false;
  });
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('alCurVal').textContent=parseFloat(d.value).toFixed(2)+' '+d.unit;
    document.getElementById('alThrDisp').textContent=(d.alarmThreshold||'–')+' '+d.unit;
    setAlarmBadge(d.alarmActive,d.alarmActive?'ALARM AKTIV – Schwellwert überschritten!':'Kein Alarm','alStatusCard');
  });
}
function saveAlarm(){
  const b={iobHost:null,iobPort:null,stateId:null,label:null,unit:null,interval:null,
           alarmEnabled:document.getElementById('alEn').checked,
           alarmThreshold:+document.getElementById('alThr').value,
           alarmAbove:document.getElementById('alMode').value==='above'};
  fetch('/api/alarm',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)})
    .then(r=>r.json()).then(d=>al('alAl',d.ok?"ok":"err",d.ok?'✓ Alarm gespeichert.':'✗ '+d.msg,3000));
}

// ── Carousel ─────────────────────────────────────────────────────────────────
let carEntries = [];

function loadCarouselUI() {
  fetch('/api/carousel').then(r=>r.json()).then(d=>{
    document.getElementById('carOn').checked = d.active;
    document.getElementById('carSec').value  = d.interval;
    document.getElementById('carCurrent').textContent =
      d.entries.length > 0 ? (d.entries[d.current]?.label||'–')+' ('+d.current+'/'+(d.entries.length-1)+')' : '–';
    document.getElementById('carActive').textContent = d.active ? '✓ Aktiv' : '✗ Inaktiv';
    document.getElementById('carCount').textContent  = d.entries.length+' / 5 Zähler';
    carEntries = d.entries;
    renderCarList();
  }).catch(e=>al('alCar','err','Fehler: '+e));
  fetch('/api/nodeinfo').then(r=>r.json()).then(d=>{
    document.getElementById('carMac').textContent = d.mac;
    document.getElementById('carStatePath').textContent = 'metermaster.0.nodes.'+d.mac;
    document.getElementById('nodeNameIn').value = d.name||"";
  }).catch(()=>{});
}

function renderCarList() {
  var el = document.getElementById('carList');
  if (!carEntries || carEntries.length === 0) {
    el.innerHTML = '<div style="color:var(--mut);font-size:.8rem">Keine Zaehler. Unten hinzufuegen.</div>';
    return;
  }
  var html="";
  for(var i=0;i<carEntries.length;i++){
    var e=carEntries[i];
    html += '<div class="ble-item" style="display:flex;align-items:center;gap:8px">';
    html += '<div style="flex:1"><div class="bn">'+(e.label||'(kein Label)')+
            ' <span style="font-size:.7rem;color:var(--mut)">'+e.unit+'</span></div>';
    html += '<div class="ba">'+e.sid+'</div></div>';
    html += '<div style="display:flex;flex-direction:column;gap:4px">';
    if(i>0)
      html += '<button class="btn sm sec" style="padding:3px 8px" onclick="carMoveUp('+i+')">&#x25B2;</button>';
    else
      html += '<div style="height:22px"></div>';
    if(i<carEntries.length-1)
      html += '<button class="btn sm sec" style="padding:3px 8px" onclick="carMoveDown('+i+')">&#x25BC;</button>';
    html += '</div>';
    html += '<button class="btn sm" style="background:linear-gradient(135deg,#991b1b,#ef4444);padding:4px 10px"'
          + ' onclick="carRemove('+i+')">&times;</button>';
    html += '</div>';
  }
  el.innerHTML = html;
}
function carAdd() {
  const sid = document.getElementById('carAddSid').value.trim();
  const lbl = document.getElementById('carAddLbl').value.trim();
  const unt = document.getElementById('carAddUnt').value.trim();
  if (!sid) { al('alCar','err','State-ID fehlt.',2000); return; }
  if (carEntries.length >= 5) { al('alCar','warn','Maximum 5 Zähler.',2000); return; }
  carEntries.push({sid, label:lbl||sid.split('.').pop(), unit:unt});
  document.getElementById('carAddSid').value="";
  document.getElementById('carAddLbl').value="";
  document.getElementById('carAddUnt').value="";
  renderCarList();
  document.getElementById('carCount').textContent = carEntries.length+' / 5 Zähler';
}

function carFromDiscover() {
  const sel = document.getElementById('discSel');
  if (!sel || sel.value==="") { al('alCar','warn','Zuerst in Einstellungen "Zähler laden" klicken.',3000); return; }
  const idx = parseInt(sel.value);
  if (isNaN(idx)||!discStates[idx]) return;
  const s = discStates[idx];
  document.getElementById('carAddSid').value = s.id;
  const parts = s.label.split(' · ');
  document.getElementById('carAddLbl').value = parts.length>=2?parts[parts.length-2]:s.label;
  document.getElementById('carAddUnt').value = s.unit;
}

function carRemove(i) { carEntries.splice(i,1); renderCarList(); document.getElementById('carCount').textContent=carEntries.length+' / 5 Zähler'; }
function carMoveUp(i) { if(i<1)return; var _t=carEntries[i]; carEntries[i]=carEntries[i-1]; carEntries[i-1]=_t; renderCarList(); }
function carMoveDown(i){ if(i>=carEntries.length-1)return; var _t=carEntries[i]; carEntries[i]=carEntries[i+1]; carEntries[i+1]=_t; renderCarList(); }

function carSave() {
  const b = {
    active: document.getElementById('carOn').checked,
    interval: +document.getElementById('carSec').value,
    entries: carEntries
  };
  fetch('/api/carousel',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)})
    .then(r=>r.json()).then(d=>{
      al('alCar',d.ok?"ok":"err",d.ok?'✓ Carousel gespeichert & aktiv.':'✗ '+d.msg,3000);
      if(d.ok) loadCarouselUI();
    });
}

function carNext() {
  fetch('/api/carousel/next').then(r=>r.json()).then(d=>{
    if(d.ok) { al('alCar','inf','→ '+d.label,1500); loadCarouselUI(); }
  });
}
function carPrev() {
  // kein eigener Endpoint – UI-seitig
  al('alCar','inf','Tipp: Carousel blättert automatisch vorwärts.',2000);
}

function saveNodeName() {
  const n = document.getElementById('nodeNameIn').value.trim();
  if (!n) return;
  fetch('/api/nodename?name='+encodeURIComponent(n))
    .then(r=>r.json()).then(d=>al('alCar',d.ok?"ok":"err",d.ok?'✓ Name gespeichert.':'✗',2000));
}

// ── OTA ───────────────────────────────────────────────────────────────────────
function evD(e,ov){e.preventDefault();document.getElementById('dropZ').classList.toggle('ov',ov);}
function drpD(e){e.preventDefault();document.getElementById('dropZ').classList.remove('ov');
  if(e.dataTransfer.files[0])upload(e.dataTransfer.files[0]);}
function upload(f){
  if(!f.name.endsWith('.bin')){al('alOta','err','Bitte eine .bin-Datei!');return;}
  document.getElementById('fName').textContent=f.name+' ('+(f.size/1024).toFixed(1)+' KB)';
  al('alOta','inf','Lade hoch…');
  const pb=document.getElementById('pb'),pi=document.getElementById('pi');
  pb.classList.add('show');pi.style.width='0%';
  const fd=new FormData();fd.append('update',f,f.name);
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=e=>{if(e.lengthComputable)pi.style.width=(e.loaded/e.total*100)+'%';};
  xhr.onload=()=>{pb.classList.remove('show');
    xhr.status===200?(al('alOta','ok','✓ Update OK! Neustart in 5 s…'),setTimeout(()=>location.reload(),6000))
    :al('alOta','err','✗ '+xhr.responseText);};
  xhr.onerror=()=>{pb.classList.remove('show');al('alOta','err','✗ Verbindungsfehler.');};
  xhr.open('POST','/update');xhr.send(fd);
}

// ── Debug / Log ───────────────────────────────────────────────────────────────
var logAutoRef = null;

function loadLog() {
  fetch('/api/log').then(r=>r.json()).then(function(entries){
    var box = document.getElementById('logBox');
    if (!entries.length) {
      box.innerHTML = '<span style="color:var(--mut)">Noch keine Einträge.</span>';
      document.getElementById('logMeta').textContent = "";
      return;
    }
    document.getElementById('logMeta').textContent = entries.length + ' Einträge';
    var html = "";
    for (var i = entries.length - 1; i >= 0; i--) {
      var e = entries[i];
      var t = (e.ts / 1000).toFixed(1);
      html += '<div style="border-bottom:1px solid rgba(255,255,255,.05);padding:2px 0">'
            + '<span style="color:#6b7280;margin-right:8px">[' + t + 's]</span>'
            + '<span>' + e.msg.replace(/</g,"&lt;") + '</span></div>';
    }
    box.innerHTML = html;
    box.scrollTop = box.scrollHeight;
  }).catch(function(){ al('alDbg','err','Log nicht ladbar'); });
  // Heap
  fetch('/api/sysinfo').then(r=>r.json()).then(function(d){
    document.getElementById('dbgHeap').textContent = (d.heapFree/1024).toFixed(1)+' KB';
    document.getElementById('dbgMinHeap').textContent = (d.heapTotal/1024).toFixed(1)+' KB gesamt';
    var pct = Math.round(d.heapFree / d.heapTotal * 100);
    document.getElementById('dbgHeapFill').style.width = pct + '%';
  }).catch(function(){});
  // Auto-refresh
  if (document.getElementById('autoRef') && document.getElementById('autoRef').checked) {
    clearTimeout(logAutoRef);
    logAutoRef = setTimeout(loadLog, 3000);
  }
}

function clearLog() {
  document.getElementById('logBox').innerHTML = '<span style="color:var(--mut)">Geleert (nur lokal).</span>';
  document.getElementById('logMeta').textContent = "";
}

function dbgFetch() {
  fetch('/api/status').then(r=>r.json()).then(function(d){
    al('alDbgAct','ok','Fetch OK: ' + d.value + ' ' + d.unit, 3000);
    loadLog();
  }).catch(function(){ al('alDbgAct','err','Fetch fehlgeschlagen', 3000); });
}

function dbgRegister() {
  fetch('/api/nodeinfo').then(r=>r.json()).then(function(d){
    al('alDbgAct','ok','Node: ' + d.mac + ' | IP: ' + d.ip, 3000);
    loadLog();
  }).catch(function(){ al('alDbgAct','err','Fehler', 3000); });
}

function dbgRestart() {
  if (!confirm('ESP32 wirklich neu starten?')) return;
  fetch('/api/restart')
  .finally(function(){ al('alDbgAct','inf','Neustart ausgelöst...', 5000); });
}

// ── Info Tab ──────────────────────────────────────────────────────────────────
var GH_U = 'MPunktBPunkt';
var GH_E = 'esp32.MeterMaster';
var GH_I = 'iobroker.metermaster';

function loadInfo() {
  fetch('/api/version').then(r=>r.json()).then(function(d){
    document.getElementById('infoVer').textContent = 'v' + d.version + ' (Build: ' + d.build + ')';
    var url = 'https://github.com/' + GH_U + '/' + GH_E;
    var el = document.getElementById('ghEspLink');
    if (el) { el.href = url; el.textContent = GH_U + '/' + GH_E + ' ↗'; }
  }).catch(function(){});
}

function checkUpdate() {
  fetch('/api/version').then(r=>r.json()).then(function(d){
    document.getElementById('otaFwCur').textContent = 'v' + d.version;
    al('alOtaCheck', 'inf', 'Prüfe GitHub…');
    fetch('https://api.github.com/repos/' + GH_U + '/' + GH_E + '/releases/latest',
      {headers: {'Accept': 'application/vnd.github.v3+json'}})
    .then(r=>r.json()).then(function(rel){
      var latest = (rel.tag_name || '').replace(/^v/, '');
      document.getElementById('otaFwNew').textContent = 'v' + latest;
      if (!latest) { al('alOtaCheck','warn','Noch kein Release auf GitHub.'); return; }
      if (latest === d.version) {
        al('alOtaCheck', 'ok', '✓ Firmware ist aktuell (v' + latest + ').', 5000);
        document.getElementById('otaDlBtn').style.display = 'none';
      } else {
        al('alOtaCheck', 'warn', '⬆ Neue Version v' + latest + ' verfügbar!');
        var asset = (rel.assets || []).find(function(a){ return a.name.endsWith('.bin'); });
        var btn = document.getElementById('otaDlBtn');
        btn.href = asset ? asset.browser_download_url : rel.html_url;
        btn.style.display = '';
        btn.textContent = asset ? '⬇ Download .bin' : '⬇ Release öffnen';
      }
    }).catch(function(){ al('alOtaCheck','err','GitHub nicht erreichbar.'); });
  });
}

function loadAdapterVersion() {
  fetch('https://api.github.com/repos/' + GH_U + '/' + GH_I + '/releases/latest',
    {headers: {'Accept': 'application/vnd.github.v3+json'}})
  .then(r=>r.json()).then(function(d){
    var el = document.getElementById('adapterVerVal');
    if (el) el.textContent = (d.tag_name || '–') + ' auf GitHub';
  }).catch(function(){
    var el = document.getElementById('adapterVerVal');
    if (el) el.textContent = '(GitHub nicht erreichbar)';
  });
}


window.onload=()=>{refreshDash();loadCfg();};
</script></body></html>
)RAW";


String buildPage() {
  String h; h.reserve(18000);
  h += FPSTR(H_HEAD); h += FPSTR(H_DB);   h += FPSTR(H_CFG);
  h += FPSTR(H_WL);   h += FPSTR(H_AL);   h += FPSTR(H_BT);
  h += FPSTR(H_CAR);  h += FPSTR(H_DBG);  h += FPSTR(H_OTA);  h += FPSTR(H_INFO); h += FPSTR(H_JS);
  return h;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Web-Handler
// ─────────────────────────────────────────────────────────────────────────────
void hApiVersion() {
  server.send(200,"application/json",
    String("{") +
    "\"version\":\"" + String(FW_VERSION) + "\"," +
    "\"build\":\"" + String(__DATE__) + " " + String(__TIME__) + "\"," +
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

void hRoot() { server.send(200,"text/html",buildPage()); }

void hApiStatus() {
  String ago = (lastFetch==0)?"Nie":(String((millis()-lastFetch)/1000)+" s");
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
    +",\"ip\":\""          + WiFi.localIP().toString() +"\"}");
}

void hApiGetSettings() {
  server.send(200,"application/json",
    "{\"iobHost\":\""     + iobHost   +"\""
    +",\"iobPort\":"      + String(iobPort)
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
  if (!doc["iobHost"].isNull()) iobHost    = doc["iobHost"].as<String>();
  if (!doc["iobPort"].isNull()) iobPort    = doc["iobPort"].as<int>();
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

void hApiOled() {
  if (!server.hasArg("plain")) { server.send(400,"application/json","{\"ok\":false,\"msg\":\"Kein Body\"}"); return; }
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc,server.arg("plain"))) { server.send(400,"application/json","{\"ok\":false,\"msg\":\"JSON\"}"); return; }
  stateId=doc["stateId"].as<String>(); meterLabel=doc["label"].as<String>(); meterUnit=doc["unit"].as<String>();
  if(!doc["oledStyle"].isNull()) oledStyle=doc["oledStyle"].as<int>();
  saveSettings(); lastFetch=0; oledValue();
  server.send(200,"application/json","{\"ok\":true}");
}

void hApiDiscover() {
  if (WiFi.status()!=WL_CONNECTED){server.send(503,"application/json","{\"ok\":false,\"msg\":\"WLAN getrennt\"}");return;}
  if (ESP.getFreeHeap()<10000){server.send(200,"application/json",
    "{\"ok\":false,\"msg\":\"Heap zu knapp ("+String(ESP.getFreeHeap()/1024)+" KB)\"}");return;}
  HTTPClient http;
  http.begin("http://"+iobHost+":"+String(iobPort)+"/getStates/metermaster.*"); http.setTimeout(8000);
  int code=http.GET();
  if(code!=200){http.end();server.send(200,"application/json",
    "{\"ok\":false,\"msg\":\"HTTP "+String(code)+" – simple-api aktiv?\"}");return;}
  String body=http.getString(); http.end();
  DynamicJsonDocument doc(16384); doc.clear();
  if(deserializeJson(doc,body)){server.send(200,"application/json","{\"ok\":false,\"msg\":\"JSON-Fehler\"}");return;}
  String r="{\"ok\":true,\"states\":["; bool first=true;
  for(JsonPair kv:doc.as<JsonObject>()){
    String sid=kv.key().c_str(); JsonVariant sv=kv.value();
    if(sv["val"].isNull()) continue;
    if(!sv["val"].is<float>()&&!sv["val"].is<int>()) continue;
    float val=sv["val"].as<float>();
    String lbl=sid; int d2=sid.indexOf('.',sid.indexOf('.')+1);
    if(d2>=0) lbl=sid.substring(d2+1); lbl.replace("."," · ");
    String unit=""; String sl=sid; sl.toLowerCase();
    if(sl.indexOf("strom")>=0||sl.indexOf("kwh")>=0) unit="kWh";
    else if(sl.indexOf("gas")>=0) unit="m³";
    else if(sl.indexOf("wasser")>=0||sl.indexOf("water")>=0) unit="m³";
    else if(sl.indexOf("waerm")>=0) unit="kWh";
    sid.replace("\"","\\\""); lbl.replace("\"","\\\"");
    if(!first)r+=",";
    r+="{\"id\":\""+sid+"\",\"val\":"+String(val,3)+",\"label\":\""+lbl+"\",\"unit\":\""+unit+"\"}";
    first=false;
  }
  r+="]}"; server.send(200,"application/json",r);
}


// GET /api/restart – Startet den ESP32 neu (nach 500 ms Verzögerung).
void hApiRestart() {
  server.send(200, "application/json", String("{"ok":true,"msg":"Neustart in 500ms"}"));
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
  server.send(200,"application/json",
    "{\"ssid\":\""     + WiFi.SSID()                    +"\","
    "\"ip\":\""        + WiFi.localIP().toString()       +"\","
    "\"mac\":\""       + WiFi.macAddress()               +"\","
    "\"rssi\":"        + String(rssi)                    +","
    "\"quality\":"     + String(q)                       +","
    "\"channel\":"     + String(WiFi.channel())          +","
    "\"heapFree\":"    + String(ESP.getFreeHeap())       +","
    "\"heapTotal\":"   + String(ESP.getHeapSize())       +","
    "\"sketchUsed\":"  + String(ESP.getSketchSize())     +","
    "\"sketchFree\":"  + String(ESP.getFreeSketchSpace())+","
    "\"chipModel\":\"" + String(ESP.getChipModel())      +"\","
    "\"cpuFreq\":"     + String(ESP.getCpuFreqMHz())     +","
    "\"uptime\":"      + String(millis()/1000)           +","
    "\"ntpSynced\":"   + String(ntpSynced?"true":"false")+","
    "\"nowTime\":\""   + nowTimeStr()                    +"\","
    "\"nowDate\":\""   + nowDateStr()                    +"\"}");
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


void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);  // LED aus

  Wire.begin(SDA_PIN, SCL_PIN);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) Serial.println("OLED nicht gefunden!");
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE);
  oledClear("MeterMaster","Node v1.3","Starte...");

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
  server.on("/api/test",     HTTP_POST, hApiTestConn);
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
  server.on("/update",       HTTP_POST, hOtaFinish, hOtaUpload);

  nodeMac = WiFi.macAddress();
  nodeMac.replace(":", "");
  addLog("Gestart. IP: " + WiFi.localIP().toString() + " MAC: " + nodeMac);
  loadCarousel();
  registerNode();
  lastRegister   = millis();
  lastConfigPoll = millis();
  server.begin();
  doFetch();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();
  // Fetch
  if (millis()-lastFetch >= (unsigned long)fetchSec*1000UL) doFetch();
  // Carousel
  handleCarousel();
  // ioBroker Heartbeat
  if (millis()-lastRegister >= REGISTER_INTERVAL) {
    registerNode(); lastRegister = millis();
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

