/*
  ============================================================================
  ESP32 HUB75 / HUB75E LED MATRIX AIRPORT DEPARTURE & ARRIVAL BOARD (HAM)
  ============================================================================
  Hardware
   - ESP32 / ESP32-S3 (empfohlen ESP32-S3 mit PSRAM fuer 256x128 Double-Buffering)
   - HUB75 / HUB75E LED Matrix Panels (8x 64x64 = 2 Reihen a 4 Panels = 256x128,
     oder 4x 64x64 = 256x64)

  Benoetigte Bibliotheken (siehe platformio.ini)
   1. ESP32 HUB75 LED MATRIX PANEL DMA Display (mrfaptastic)
   2. Adafruit GFX Library
   3. ArduinoJson (v7.x oder v6.x)

  AirLabs API Key: include/secrets.example.h nach include/secrets.h kopieren
  oder im Web-Interface des Boards eintragen.
  ============================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ESP32-HUB75-VirtualMatrixPanel_T.hpp>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// ============================================================================
// 1. AIRLABS API KEY (Standardwert aus include/secrets.h, ueberschreibbar im Web-Interface)
// ============================================================================
#ifndef AIRLABS_API_KEY
#define AIRLABS_API_KEY ""
#endif
static const char *API_KEY_PLACEHOLDER = "DEIN_AIRLABS_API_KEY_HIER";

// ============================================================================
// 2. HUB75 / HUB75E PANEL KONFIGURATION
// ============================================================================
#define BOARD_PANEL_RES_X 64
#define BOARD_PANEL_RES_Y 64
#define BOARD_PANEL_COLS 4 // 4 * 64 = 256 Pixel Breite
#define BOARD_SCREEN_W (BOARD_PANEL_RES_X * BOARD_PANEL_COLS)

// Verkettung der Panels bei 2 Reihen (256x128). Bei einer Reihe spielt der Wert keine Rolle.
// CHAIN_TOP_LEFT_DOWN = Serpentine (untere Reihe ueber Kopf verkabelt),
// CHAIN_TOP_LEFT_DOWN_ZZ = Zickzack (alle Panels aufrecht, lange Kabel zurueck)
#define BOARD_CHAIN_TYPE CHAIN_TOP_LEFT_DOWN

// 1 = Double-Buffering (flackerfrei, braucht viel DMA-RAM -> nur ESP32-S3 empfohlen)
#ifndef BOARD_DOUBLE_BUFFER
#define BOARD_DOUBLE_BUFFER 0
#endif

// Eindeutige Pin-Namen (verhindert Makro-Kollisionen mit ESP32-HUB75-MatrixPanel-I2S-DMA.h)
#define BOARD_R1_PIN 25
#define BOARD_G1_PIN 26
#define BOARD_B1_PIN 27
#define BOARD_R2_PIN 14
#define BOARD_G2_PIN 12
#define BOARD_B2_PIN 13
#define BOARD_A_PIN 23
#define BOARD_B_PIN 19
#define BOARD_C_PIN 5
#define BOARD_D_PIN 17
#define BOARD_E_PIN 18 // Wichtig fuer HUB75E (64px hohe Panels)!
#define BOARD_LAT_PIN 4
#define BOARD_OE_PIN 15
#define BOARD_CLK_PIN 16

using BoardDisplay = VirtualMatrixPanel_T<BOARD_CHAIN_TYPE>;
MatrixPanel_I2S_DMA *dma_display = nullptr;
BoardDisplay *gfx = nullptr;

// ============================================================================
// 3. FARBDEFINITIONEN (RGB565)
// ============================================================================
uint16_t COL_BG;
uint16_t COL_HEADER_BG;
uint16_t COL_AMBER;
uint16_t COL_WHITE;
uint16_t COL_GREEN;
uint16_t COL_RED;
uint16_t COL_CYAN;
uint16_t COL_BELT;
uint16_t COL_DIM;
uint16_t COL_LINE;

// ============================================================================
// 4. EINSTELLUNGEN & DATENSTRUKTUREN
// ============================================================================
Preferences prefs;
WebServer server(80);

struct BoardConfig {
  String wifiSsid;
  String wifiPass;
  String apiKey;
  String iataCode;
  int matrixHeight;      // 128 oder 64 (Aenderung erfordert Neustart)
  int switchSeconds;     // Wechselintervall Abflug <-> Ankunft (Sekunden)
  int pollMinutes;       // API-Abrufintervall (Minuten)
  int brightness;        // 10..255
  bool codeshareMarquee; // false = Operating Only, true = Smooth Marquee
  String timezone;       // POSIX Zeitzone (Standard CET/CEST fuer Deutschland)
} cfg;

struct FlightEntry {
  String schedTime;       // "14:50"
  String operatingFlight; // "LH2071"
  String allFlights;      // "LH2071  OS7241  LX3611"
  String cityName;        // "PALMA DE MALLORCA"
  String gateOrBelt;      // "C7" oder "14"
  bool isBaggageBelt;
  String statusLabel;     // ON TIME, BOARDING, EST 15:16, DEPARTED, LANDED
  uint16_t statusColor;
  time_t sortTs;
};

#define MAX_FLIGHTS 12
FlightEntry depFlights[MAX_FLIGHTS];
int depCount = 0;

FlightEntry arrFlights[MAX_FLIGHTS];
int arrCount = 0;

bool showDepartures = true;
bool isLiveApiData = false;
bool apModeActive = false;
bool animNeeded = false; // wird beim Rendern gesetzt, wenn ein Text scrollen muss
String lastApiStatus = "Initialisiere...";
String lastClockStr = "";

unsigned long lastSwitchMs = 0;
unsigned long lastPollMs = 0;
unsigned long lastAnimMs = 0;
uint32_t animFrame = 0;

// ============================================================================
// 5. IATA -> ENGLISCHE STAEDTENAMEN
// ============================================================================
struct CityMap {
  const char *iata;
  const char *name;
};
const CityMap CITY_LOOKUP[] = {
    {"HAM", "HAMBURG"}, {"FRA", "FRANKFURT"}, {"MUC", "MUNICH"}, {"BER", "BERLIN"},
    {"DUS", "DUSSELDORF"}, {"CGN", "COLOGNE"}, {"STR", "STUTTGART"}, {"NUE", "NUREMBERG"},
    {"ZRH", "ZURICH"}, {"GVA", "GENEVA"}, {"BSL", "BASEL"}, {"VIE", "VIENNA"},
    {"SZG", "SALZBURG"}, {"INN", "INNSBRUCK"}, {"LHR", "LONDON"}, {"LGW", "LONDON"},
    {"STN", "LONDON"}, {"LCY", "LONDON"}, {"MAN", "MANCHESTER"}, {"EDI", "EDINBURGH"},
    {"DUB", "DUBLIN"}, {"CDG", "PARIS"}, {"ORY", "PARIS"}, {"NCE", "NICE"},
    {"LYS", "LYON"}, {"AMS", "AMSTERDAM"}, {"BRU", "BRUSSELS"}, {"LUX", "LUXEMBOURG"},
    {"CPH", "COPENHAGEN"}, {"BLL", "BILLUND"}, {"OSL", "OSLO"}, {"BGO", "BERGEN"},
    {"ARN", "STOCKHOLM"}, {"GOT", "GOTHENBURG"}, {"HEL", "HELSINKI"}, {"RIX", "RIGA"},
    {"TLL", "TALLINN"}, {"VNO", "VILNIUS"}, {"KEF", "REYKJAVIK"}, {"MAD", "MADRID"},
    {"BCN", "BARCELONA"}, {"PMI", "PALMA DE MALLORCA"}, {"AGP", "MALAGA"},
    {"ALC", "ALICANTE"}, {"VLC", "VALENCIA"}, {"IBZ", "IBIZA"}, {"LPA", "GRAN CANARIA"},
    {"TFS", "TENERIFE"}, {"LIS", "LISBON"}, {"OPO", "PORTO"}, {"FAO", "FARO"},
    {"FCO", "ROME"}, {"MXP", "MILAN"}, {"LIN", "MILAN"}, {"VCE", "VENICE"},
    {"BLQ", "BOLOGNA"}, {"NAP", "NAPLES"}, {"CTA", "CATANIA"}, {"ATH", "ATHENS"},
    {"SKG", "THESSALONIKI"}, {"HER", "HERAKLION"}, {"RHO", "RHODES"}, {"LCA", "LARNACA"},
    {"MLA", "MALTA"}, {"WAW", "WARSAW"}, {"KRK", "KRAKOW"}, {"GDN", "GDANSK"},
    {"PRG", "PRAGUE"}, {"BUD", "BUDAPEST"}, {"OTP", "BUCHAREST"}, {"SOF", "SOFIA"},
    {"ZAG", "ZAGREB"}, {"SPU", "SPLIT"}, {"DBV", "DUBROVNIK"}, {"BEG", "BELGRADE"},
    {"IST", "ISTANBUL"}, {"SAW", "ISTANBUL"}, {"AYT", "ANTALYA"}, {"ADB", "IZMIR"},
    {"DXB", "DUBAI"}, {"DOH", "DOHA"}, {"HRG", "HURGHADA"}, {"JFK", "NEW YORK"},
    {"EWR", "NEWARK"}, {"YYZ", "TORONTO"}, {"SIN", "SINGAPORE"}, {"HND", "TOKYO"}};

String resolveCityName(const String &iata) {
  String code = iata;
  code.toUpperCase();
  int count = sizeof(CITY_LOOKUP) / sizeof(CITY_LOOKUP[0]);
  for (int i = 0; i < count; i++) {
    if (code == CITY_LOOKUP[i].iata) {
      return String(CITY_LOOKUP[i].name);
    }
  }
  return code.length() > 0 ? code : "---";
}

// ============================================================================
// 6. HILFSFUNKTIONEN FUER ZEIT & KONFIGURATION
// ============================================================================
void loadConfig() {
  prefs.begin("airport_cfg", true);
  cfg.wifiSsid = prefs.getString("ssid", "");
  cfg.wifiPass = prefs.getString("pass", "");
  cfg.apiKey = prefs.getString("api_key", AIRLABS_API_KEY);
  cfg.iataCode = prefs.getString("iata", "HAM");
  cfg.matrixHeight = prefs.getInt("height", 128);
  cfg.switchSeconds = prefs.getInt("sw_sec", 15);
  cfg.pollMinutes = prefs.getInt("poll_min", 10);
  cfg.brightness = prefs.getInt("bright", 90);
  cfg.codeshareMarquee = prefs.getBool("cs_marq", false);
  cfg.timezone = prefs.getString("tz", "CET-1CEST,M3.5.0,M10.5.0/3");
  prefs.end();

  if (cfg.matrixHeight != 64) cfg.matrixHeight = 128;
}

void saveConfig() {
  prefs.begin("airport_cfg", false);
  prefs.putString("ssid", cfg.wifiSsid);
  prefs.putString("pass", cfg.wifiPass);
  prefs.putString("api_key", cfg.apiKey);
  prefs.putString("iata", cfg.iataCode);
  prefs.putInt("height", cfg.matrixHeight);
  prefs.putInt("sw_sec", cfg.switchSeconds);
  prefs.putInt("poll_min", cfg.pollMinutes);
  prefs.putInt("bright", cfg.brightness);
  prefs.putBool("cs_marq", cfg.codeshareMarquee);
  prefs.putString("tz", cfg.timezone);
  prefs.end();
}

String getCurrentClockHHMM() {
  struct tm timeinfo;
  // Timeout 0: kein Blockieren, solange NTP noch nicht synchronisiert ist
  if (!getLocalTime(&timeinfo, 0)) {
    return "--:--";
  }
  char buf[6];
  strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
  return String(buf);
}

String extractHHMM(const char *dtStr) {
  if (!dtStr) return "--:--";
  String s(dtStr);
  int spaceIdx = s.indexOf(' ');
  if (spaceIdx >= 0 && (int)s.length() >= spaceIdx + 6) {
    return s.substring(spaceIdx + 1, spaceIdx + 6);
  }
  return s.length() >= 5 ? s.substring(0, 5) : "--:--";
}

String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

// ============================================================================
// 7. DEMO-DATEN (FALLS KEY NOCH NICHT GESETZT ODER KEIN INTERNET)
// ============================================================================
void loadDemoData() {
  time_t now;
  time(&now);

  depCount = 7;
  depFlights[0] = {"14:25", "LH2061", "LH2061", "MUNICH", "A19", false, "DEPARTED", COL_CYAN, now - 120};
  depFlights[1] = {"14:50", "AZ3855", "AZ3855  BT1057", "ZURICH", "C7", false, "EST 15:16", COL_AMBER, now + 480};
  depFlights[2] = {"15:05", "LH2071", "LH2071  OS7241  LX3611", "MUNICH", "A17", false, "BOARDING", COL_GREEN, now + 1080};
  depFlights[3] = {"15:20", "BA965", "BA965  IB7412", "LONDON", "C04", false, "ON TIME", COL_GREEN, now + 1920};
  depFlights[4] = {"15:35", "KL1780", "KL1780", "AMSTERDAM", "B52", false, "ON TIME", COL_GREEN, now + 2700};
  depFlights[5] = {"15:50", "SK652", "SK652  LH6214", "COPENHAGEN", "A22", false, "ON TIME", COL_GREEN, now + 3480};
  depFlights[6] = {"16:10", "EW7584", "EW7584", "PALMA DE MALLORCA", "B44", false, "ON TIME", COL_GREEN, now + 4320};

  arrCount = 7;
  arrFlights[0] = {"14:30", "LX1056", "LX1056", "ZURICH", "14", true, "LANDED", COL_GREEN, now - 240};
  arrFlights[1] = {"14:55", "LH014", "LH014  UA8841  AC9102", "FRANKFURT", "04", true, "APPROACH", COL_CYAN, now + 420};
  arrFlights[2] = {"15:10", "EW7041", "EW7041", "VIENNA", "09", true, "EST 15:28", COL_AMBER, now + 1140};
  arrFlights[3] = {"15:25", "FR408", "FR408", "LONDON", "02", true, "APPROACH", COL_CYAN, now + 1800};
  arrFlights[4] = {"15:40", "AY1423", "AY1423", "HELSINKI", "11", true, "ON TIME", COL_GREEN, now + 2700};
  arrFlights[5] = {"15:55", "IB3272", "IB3272", "MADRID", "07", true, "ON TIME", COL_GREEN, now + 3600};
  arrFlights[6] = {"16:15", "EW7585", "EW7585", "PALMA DE MALLORCA", "15", true, "ON TIME", COL_GREEN, now + 4500};

  isLiveApiData = false;
}

// ============================================================================
// 8. AIRLABS API ABRUFEN & PARSEN (INKL. 5-MIN-REGEL & CODESHARE-GRUPPIERUNG)
// ============================================================================
bool fetchAirLabsEndpoint(bool isDeparture, FlightEntry *targetArr, int &targetCount) {
  String key = cfg.apiKey;
  key.trim();
  if (key.length() < 10 || key == API_KEY_PLACEHOLDER) {
    lastApiStatus = "Demo-Modus (Kein API-Key eingetragen)";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // AirLabs HTTPS ohne Zertifikats-Pinning

  HTTPClient http;
  http.useHTTP10(true); // verhindert Chunked-Encoding, das den Stream-Parser stoeren wuerde
  http.setTimeout(10000);
  String param = isDeparture ? "dep_iata=" : "arr_iata=";
  String url = "https://airlabs.co/api/v9/schedules?" + param + cfg.iataCode + "&api_key=" + key;

  if (!http.begin(client, url)) {
    lastApiStatus = "HTTPS Verbindung fehlgeschlagen";
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    lastApiStatus = "HTTP Fehler " + String(httpCode);
    http.end();
    return false;
  }

  // Filter spart RAM auf dem ESP32 (kompatibel mit ArduinoJson v6 und v7)
#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument filter;
#else
  StaticJsonDocument<512> filter;
#endif
  JsonObject fObj = filter["response"][0].to<JsonObject>();
  fObj["flight_iata"] = true;
  fObj["cs_flight_iata"] = true;
  fObj["dep_iata"] = true;
  fObj["arr_iata"] = true;
  fObj["dep_gate"] = true;
  fObj["arr_gate"] = true;
  fObj["arr_baggage"] = true;
  fObj["dep_time"] = true;
  fObj["arr_time"] = true;
  fObj["dep_estimated"] = true;
  fObj["arr_estimated"] = true;
  fObj["dep_time_ts"] = true;
  fObj["arr_time_ts"] = true;
  fObj["dep_actual_ts"] = true;
  fObj["arr_actual_ts"] = true;
  fObj["status"] = true;
  fObj["delayed"] = true;
  fObj["dep_delayed"] = true;
  fObj["arr_delayed"] = true;

#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument doc;
#else
  DynamicJsonDocument doc(24576);
#endif
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    lastApiStatus = "JSON Parse Fehler " + String(err.c_str());
    return false;
  }

  JsonArray arr = doc["response"].as<JsonArray>();
  if (arr.isNull()) {
    lastApiStatus = "Leere API-Antwort";
    return false;
  }

  time_t now;
  time(&now);
  targetCount = 0;

  for (JsonObject item : arr) {
    const char *fIataRaw = item["flight_iata"] | "";
    String fIata(fIataRaw);
    fIata.trim();
    if (fIata.length() == 0) continue;

    String status(item["status"] | "scheduled");
    status.toLowerCase();

    time_t schedTs = isDeparture ? (item["dep_time_ts"] | 0L) : (item["arr_time_ts"] | 0L);
    time_t actualTs = isDeparture ? (item["dep_actual_ts"] | 0L) : (item["arr_actual_ts"] | 0L);
    time_t refTs = actualTs > 0 ? actualTs : schedTs;

    // 5-Minuten-Regel: Wenn gestartet, maximal 5 Minuten (300s) anzeigen
    if (isDeparture) {
      if ((status == "active" || status == "landed" || actualTs > 0) && refTs > 0 && (now - refTs > 300)) {
        continue;
      }
      if (schedTs > 0 && (now - schedTs > 1800) && status == "scheduled") {
        continue;
      }
    } else {
      if (status == "landed" && refTs > 0 && (now - refTs > 600)) {
        continue;
      }
      if (schedTs > 0 && (now - schedTs > 2700) && status != "active") {
        continue;
      }
    }

    String remoteIata = isDeparture ? String(item["arr_iata"] | "") : String(item["dep_iata"] | "");
    String city = resolveCityName(remoteIata);
    String schedHHMM = isDeparture ? extractHHMM(item["dep_time"] | nullptr) : extractHHMM(item["arr_time"] | nullptr);
    const char *csRaw = item["cs_flight_iata"] | nullptr;
    bool hasCs = csRaw && strlen(csRaw) > 0 && String(csRaw) != fIata;

    // Codeshare-Erkennung: Gleiche Stadt & gleiche Uhrzeit zusammenfassen
    int existingIdx = -1;
    for (int i = 0; i < targetCount; i++) {
      if (targetArr[i].cityName == city && targetArr[i].schedTime == schedHHMM) {
        existingIdx = i;
        break;
      }
    }

    if (existingIdx >= 0) {
      // Codeshare an bestehenden Eintrag anhaengen
      if (targetArr[existingIdx].allFlights.indexOf(fIata) < 0) {
        targetArr[existingIdx].allFlights += "  " + fIata;
      }
      // Falls dieser Eintrag der Operating Carrier ist (kein cs_flight_iata gesetzt), nach vorn setzen
      if (!hasCs) {
        targetArr[existingIdx].operatingFlight = fIata;
      }
      continue;
    }

    FlightEntry entry;
    entry.schedTime = schedHHMM;
    entry.operatingFlight = fIata;
    entry.allFlights = fIata;
    if (hasCs) {
      entry.allFlights += "  " + String(csRaw);
    }
    entry.cityName = city;
    entry.sortTs = schedTs;

    // Gate (ohne Terminal) bzw. Belt bei Arrivals
    if (isDeparture) {
      const char *g = item["dep_gate"] | nullptr;
      entry.gateOrBelt = (g && strlen(g) > 0) ? String(g) : "-";
      entry.isBaggageBelt = false;
    } else {
      const char *b = item["arr_baggage"] | nullptr;
      const char *g = item["arr_gate"] | nullptr;
      if (b && strlen(b) > 0) {
        entry.gateOrBelt = String(b);
        entry.isBaggageBelt = true;
      } else if (g && strlen(g) > 0) {
        entry.gateOrBelt = String(g);
        entry.isBaggageBelt = false;
      } else {
        entry.gateOrBelt = "-";
        entry.isBaggageBelt = false;
      }
    }

    int delayMin = isDeparture ? (item["dep_delayed"] | (item["delayed"] | 0))
                               : (item["arr_delayed"] | (item["delayed"] | 0));
    String estHHMM = isDeparture ? extractHHMM(item["dep_estimated"] | nullptr) : extractHHMM(item["arr_estimated"] | nullptr);

    if (status == "cancelled") {
      entry.statusLabel = "CANCELLED";
      entry.statusColor = COL_RED;
    } else if (status == "landed") {
      entry.statusLabel = "LANDED";
      entry.statusColor = COL_GREEN;
    } else if (status == "active") {
      if (isDeparture) {
        entry.statusLabel = "DEPARTED";
        entry.statusColor = COL_CYAN;
      } else if (delayMin > 10 && estHHMM != "--:--") {
        entry.statusLabel = "EST " + estHHMM;
        entry.statusColor = COL_AMBER;
      } else {
        entry.statusLabel = "APPROACH";
        entry.statusColor = COL_CYAN;
      }
    } else if (delayMin > 5 && estHHMM != "--:--") {
      entry.statusLabel = "EST " + estHHMM;
      entry.statusColor = COL_AMBER;
    } else {
      entry.statusLabel = "ON TIME";
      entry.statusColor = COL_GREEN;
    }

    if (targetCount < MAX_FLIGHTS) {
      targetArr[targetCount++] = entry;
    } else {
      // Liste voll: spaetesten Flug verdraengen, falls der neue frueher startet
      int latest = 0;
      for (int i = 1; i < targetCount; i++) {
        if (targetArr[i].sortTs > targetArr[latest].sortTs) latest = i;
      }
      if (entry.sortTs < targetArr[latest].sortTs) targetArr[latest] = entry;
    }
  }

  // Chronologisch sortieren
  for (int i = 0; i < targetCount - 1; i++) {
    for (int j = i + 1; j < targetCount; j++) {
      if (targetArr[j].sortTs < targetArr[i].sortTs) {
        FlightEntry tmp = targetArr[i];
        targetArr[i] = targetArr[j];
        targetArr[j] = tmp;
      }
    }
  }

  lastApiStatus = "Live OK (" + cfg.iataCode + ")";
  return true;
}

void updateAllFlightData() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!isLiveApiData) loadDemoData();
    return;
  }
  bool okDep = fetchAirLabsEndpoint(true, depFlights, depCount);
  bool okArr = fetchAirLabsEndpoint(false, arrFlights, arrCount);
  if (okDep && okArr) {
    isLiveApiData = true;
  } else if (!isLiveApiData) {
    // Bei kurzzeitigem Fehler bleiben bereits geladene Live-Daten stehen
    loadDemoData();
  }
}

// ============================================================================
// 9. PIXEL-ART ICONS & LED-MATRIX RENDERING (256x128 & 256x64)
// ============================================================================
void drawPlaneIcon(int x, int y, bool takeoff, uint16_t color) {
  // Runway-Linie unten
  gfx->drawFastHLine(x, y + 11, 13, color);
  if (takeoff) {
    // Steigendes Flugzeug
    gfx->drawLine(x + 2, y + 8, x + 11, y + 4, color);
    gfx->drawLine(x + 2, y + 9, x + 12, y + 5, color);
    gfx->drawLine(x + 5, y + 2, x + 8, y + 6, color);
    gfx->drawLine(x + 1, y + 6, x + 3, y + 8, color);
  } else {
    // Sinkendes Flugzeug
    gfx->drawLine(x + 2, y + 5, x + 11, y + 9, color);
    gfx->drawLine(x + 2, y + 6, x + 12, y + 10, color);
    gfx->drawLine(x + 6, y + 2, x + 8, y + 7, color);
    gfx->drawLine(x + 1, y + 3, x + 3, y + 6, color);
  }
}

// Zeichnet Text innerhalb einer festen Pixelbox mit optionalem Scroll-Effekt
void drawClippedCellText(int x, int y, int maxW, const String &text, uint16_t color, bool pauseAndReveal) {
  int charW = 6; // Standard Adafruit GFX 5x7 Font = 6px pro Zeichen
  int textPx = text.length() * charW;

  if (textPx <= maxW) {
    gfx->setTextColor(color);
    gfx->setCursor(x, y);
    gfx->print(text);
    return;
  }

  animNeeded = true;

  int offsetPx = 0;
  String loopStr = text + "   ";
  if (pauseAndReveal) {
    // Erst kurz statisch anzeigen, dann sanft bis zum Ende scrollen (z.B. PALMA DE MALLORCA)
    int overflow = textPx - maxW + 4;
    int cycle = animFrame % (overflow + 55);
    if (cycle < 25) {
      offsetPx = 0;
    } else if (cycle < 25 + overflow) {
      offsetPx = cycle - 25;
    } else {
      offsetPx = overflow;
    }
  } else {
    // Endlos-Laufschrift fuer Codeshare-Flugnummern
    int loopW = (int)loopStr.length() * charW;
    offsetPx = animFrame % loopW;
  }

  // Pixelgenaues Clipping auf Spaltenbreite maxW
  gfx->setTextColor(color);
  int startChar = offsetPx / charW;
  int subPixelShift = offsetPx % charW;
  int visibleChars = (maxW / charW) + 2;

  for (int i = 0; i < visibleChars; i++) {
    int idx = startChar + i;
    char c = ' ';
    if (pauseAndReveal) {
      if (idx >= 0 && idx < (int)text.length()) c = text[idx];
    } else {
      c = loopStr[idx % loopStr.length()];
    }
    int drawX = x + (i * charW) - subPixelShift;
    if (drawX >= x && drawX <= x + maxW - 5) {
      gfx->drawChar(drawX, y, c, color, COL_BG, 1);
    }
  }
}

void drawBoard() {
  gfx->fillScreen(COL_BG);
  gfx->setTextWrap(false);
  gfx->setTextSize(1);
  animNeeded = false;

  // --------------------------------------------------------------------------
  // 1. KOPFZEILE (0..18 px): Icon + Title | Uhrzeit (mittig) | IATA + Status-Dot
  // --------------------------------------------------------------------------
  gfx->fillRect(0, 0, BOARD_SCREEN_W, 18, COL_HEADER_BG);
  gfx->drawFastHLine(0, 18, BOARD_SCREEN_W, COL_AMBER);

  drawPlaneIcon(3, 3, showDepartures, COL_AMBER);

  gfx->setTextColor(COL_AMBER);
  gfx->setCursor(20, 5);
  gfx->print(apModeActive ? "WIFI SETUP" : (showDepartures ? "DEPARTURES" : "ARRIVALS"));

  // Uhrzeit exakt mittig zwischen Titel und Airport-Kenner (im Setup-Modus --:--)
  String clockStr = apModeActive ? "--:--" : getCurrentClockHHMM();
  gfx->setTextColor(COL_AMBER);
  gfx->setCursor(134, 5);
  gfx->print(clockStr);

  // Airport-Kenner Badge (rechts)
  int badgeX = 222;
  gfx->fillRect(badgeX, 3, 22, 12, COL_AMBER);
  gfx->setTextColor(COL_BG);
  gfx->setCursor(badgeX + 2, 5);
  gfx->print(cfg.iataCode);

  // Status-Punkt neben Airport-Kenner (Gruen = Live, Bernstein = Setup, Dim = Demo)
  uint16_t dotCol = apModeActive ? COL_AMBER : (isLiveApiData ? COL_GREEN : COL_DIM);
  gfx->fillCircle(249, 9, 3, dotCol);

  // Falls WLAN noch nicht konfiguriert ist: Hinweis im gleichen Tafel-Layout anzeigen
  if (apModeActive) {
    if (cfg.matrixHeight >= 128) {
      gfx->fillRect(0, 19, BOARD_SCREEN_W, 10, COL_HEADER_BG);
      gfx->drawFastHLine(0, 29, BOARD_SCREEN_W, COL_LINE);
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(4, 21);
      gfx->print("STATUS");
      gfx->setCursor(78, 21);
      gfx->print("CONNECTION INFO");

      gfx->setTextColor(COL_AMBER);
      gfx->setCursor(4, 38);
      gfx->print("SETUP");
      gfx->setTextColor(COL_WHITE);
      gfx->setCursor(52, 38);
      gfx->print("Please join WiFi & open website");
      gfx->drawFastHLine(0, 51, BOARD_SCREEN_W, COL_LINE);

      gfx->setTextColor(COL_AMBER);
      gfx->setCursor(4, 58);
      gfx->print("WIFI");
      gfx->setTextColor(COL_CYAN);
      gfx->setCursor(52, 58);
      gfx->print("AIRPORT-BOARD-SETUP");
      gfx->drawFastHLine(0, 71, BOARD_SCREEN_W, COL_LINE);

      gfx->setTextColor(COL_AMBER);
      gfx->setCursor(4, 78);
      gfx->print("WEB");
      gfx->setTextColor(COL_GREEN);
      gfx->setCursor(52, 78);
      gfx->print("http://192.168.4.1");
      gfx->drawFastHLine(0, 91, BOARD_SCREEN_W, COL_LINE);

      gfx->setTextColor(COL_DIM);
      gfx->setCursor(4, 100);
      gfx->print("Open URL in browser for WiFi settings");
    } else {
      // Kompakte 3-Zeilen-Ansicht fuer 256x64
      gfx->setTextColor(COL_WHITE);
      gfx->setCursor(4, 23);
      gfx->print("Please join WiFi for settings");
      gfx->drawFastHLine(0, 34, BOARD_SCREEN_W, COL_LINE);

      gfx->setTextColor(COL_AMBER);
      gfx->setCursor(4, 38);
      gfx->print("WIFI");
      gfx->setTextColor(COL_CYAN);
      gfx->setCursor(40, 38);
      gfx->print("AIRPORT-BOARD-SETUP");
      gfx->drawFastHLine(0, 49, BOARD_SCREEN_W, COL_LINE);

      gfx->setTextColor(COL_AMBER);
      gfx->setCursor(4, 53);
      gfx->print("URL");
      gfx->setTextColor(COL_GREEN);
      gfx->setCursor(40, 53);
      gfx->print("http://192.168.4.1");
    }
    return;
  }

  // --------------------------------------------------------------------------
  // 2. SPALTENKOEPFE (nur bei 256x128 Hoehe sichtbar)
  //    Spalten-X: TIME=2, FLIGHT=36, DEST=78, GATE/BELT=164, STATUS=214
  // --------------------------------------------------------------------------
  int startY = 20;
  int rowHeight = 15;
  int maxRows = 3;

  if (cfg.matrixHeight >= 128) {
    gfx->fillRect(0, 19, BOARD_SCREEN_W, 10, COL_HEADER_BG);
    gfx->drawFastHLine(0, 29, BOARD_SCREEN_W, COL_LINE);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(2, 21);
    gfx->print("TIME");
    gfx->setCursor(36, 21);
    gfx->print("FLIGHT");
    gfx->setCursor(78, 21);
    gfx->print(showDepartures ? "DEST" : "FROM");
    gfx->setCursor(164, 21);
    gfx->print(showDepartures ? "GATE" : "BELT");
    gfx->setCursor(214, 21);
    gfx->print("STATUS");
    startY = 31;
    rowHeight = 14;
    maxRows = 7;
  }

  // --------------------------------------------------------------------------
  // 3. FLUGZEILEN RENDERN
  // --------------------------------------------------------------------------
  FlightEntry *list = showDepartures ? depFlights : arrFlights;
  int count = showDepartures ? depCount : arrCount;

  if (count == 0) {
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(64, startY + 14);
    gfx->print("NO FLIGHTS SCHEDULED");
    return;
  }

  int visible = (count < maxRows) ? count : maxRows;
  for (int i = 0; i < visible; i++) {
    int y = startY + (i * rowHeight);
    int textY = y + 3;
    FlightEntry &f = list[i];

    // TIME (x=2)
    gfx->setTextColor(COL_AMBER);
    gfx->setCursor(2, textY);
    gfx->print(f.schedTime);

    // FLIGHT (x=36, maxW=38px)
    if (cfg.codeshareMarquee && f.allFlights.indexOf(' ') > 0) {
      drawClippedCellText(36, textY, 38, f.allFlights, COL_WHITE, false);
    } else {
      drawClippedCellText(36, textY, 38, f.operatingFlight, COL_WHITE, false);
    }

    // DEST / FROM (x=78, maxW=82px - Pause & Scroll bei PALMA DE MALLORCA)
    drawClippedCellText(78, textY, 82, f.cityName, COL_AMBER, true);

    // GATE / BELT (x=164, linksbuendig)
    gfx->setTextColor(f.isBaggageBelt ? COL_BELT : COL_CYAN);
    gfx->setCursor(164, textY);
    gfx->print(f.gateOrBelt);

    // STATUS (rechtsbuendig bis x=255, genuegend Platz fuer 9 Zeichen wie CANCELLED)
    int stWidth = (int)f.statusLabel.length() * 6;
    int stX = 255 - stWidth;
    if (stX < 192) stX = 192;
    gfx->setTextColor(f.statusColor);
    gfx->setCursor(stX, textY);
    gfx->print(f.statusLabel);

    // Dezente Trennlinie
    if (i < visible - 1) {
      gfx->drawFastHLine(0, y + rowHeight - 1, BOARD_SCREEN_W, COL_LINE);
    }
  }
}

void renderMatrixDisplay() {
  if (!gfx) return;
  drawBoard();
  dma_display->flipDMABuffer(); // No-Op ohne Double-Buffering
}

// ============================================================================
// 10. WEB-INTERFACE MIT WLAN-SCANNER DROPDOWN & BOARD-EINSTELLUNGEN
// ============================================================================
String buildHtmlPage(const String &scannedOptions, const String &bannerMsg = "") {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 HUB75 Airport Board</title>
  <style>
    body { font-family: system-ui, -apple-system, sans-serif; background: #0f1115; color: #f4f4f5; margin: 0; padding: 16px; }
    .card { max-width: 520px; margin: 0 auto; background: #181a20; border: 1px solid #2b2e38; border-radius: 12px; padding: 20px; box-shadow: 0 10px 30px rgba(0,0,0,0.5); }
    h1 { font-size: 1.2rem; color: #ffbe0b; margin-top: 0; display: flex; justify-content: space-between; align-items: center; }
    .badge { font-size: 0.75rem; padding: 3px 8px; border-radius: 99px; background: #27272a; color: #39ff14; }
    label { display: block; font-size: 0.8rem; color: #a1a1aa; margin: 12px 0 4px; font-weight: 600; }
    input, select { width: 100%; box-sizing: border-box; padding: 10px; border-radius: 8px; border: 1px solid #3f3f46; background: #09090b; color: #fff; font-size: 0.95rem; }
    .row { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
    .btn { width: 100%; margin-top: 18px; padding: 12px; border: none; border-radius: 8px; background: #ffbe0b; color: #000; font-weight: 700; font-size: 1rem; cursor: pointer; }
    .btn-sec { background: #27272a; color: #fff; margin-top: 8px; font-size: 0.85rem; }
    .banner { background: #14532d; border: 1px solid #22c55e; color: #bbf7d0; padding: 10px; border-radius: 8px; margin-bottom: 14px; font-size: 0.85rem; }
    .status { font-size: 0.8rem; color: #a1a1aa; margin-top: 12px; text-align: center; }
  </style>
</head>
<body>
  <div class="card">
    <h1>✈ ESP32 Airport Matrix <span class="badge">)rawliteral";

  html += isLiveApiData ? "LIVE API" : (apModeActive ? "SETUP" : "DEMO");
  html += R"rawliteral(</span></h1>)rawliteral";

  if (bannerMsg.length() > 0) {
    html += "<div class='banner'>" + htmlEscape(bannerMsg) + "</div>";
  }

  html += R"rawliteral(
    <form method="POST" action="/save">
      <label for="ssid">WLAN-Netzwerk (in Reichweite erkannt)</label>
      <select id="ssid" name="ssid">
        <option value="">-- WLAN auswählen (leer lassen = unverändert) --</option>
        )rawliteral" + scannedOptions + R"rawliteral(
      </select>

      <label for="pass">WLAN-Passwort</label>
      <input type="password" id="pass" name="pass" placeholder="Passwort eingeben (leer = gespeichert behalten)">

      <label for="api_key">AirLabs API-Key (leer = unverändert)</label>
      <input type="password" id="api_key" name="api_key" placeholder=")rawliteral" + String(cfg.apiKey.length() >= 10 ? "gespeichert" : "nicht gesetzt") + R"rawliteral(">

      <div class="row">
        <div>
          <label for="iata">Flughafen IATA</label>
          <input type="text" id="iata" name="iata" maxlength="3" value=")rawliteral" + htmlEscape(cfg.iataCode) + R"rawliteral(">
        </div>
        <div>
          <label for="height">Matrix Auflösung</label>
          <select id="height" name="height">
            <option value="128" )rawliteral" + String(cfg.matrixHeight == 128 ? "selected" : "") + R"rawliteral(>256x128 (7 Zeilen)</option>
            <option value="64" )rawliteral" + String(cfg.matrixHeight == 64 ? "selected" : "") + R"rawliteral(>256x64 (3 Zeilen)</option>
          </select>
        </div>
      </div>

      <div class="row">
        <div>
          <label for="sw_sec">Wechsel Abflug/Ankunft (Sek.)</label>
          <input type="number" id="sw_sec" name="sw_sec" min="4" max="120" value=")rawliteral" + String(cfg.switchSeconds) + R"rawliteral(">
        </div>
        <div>
          <label for="poll_min">API Update (Min.)</label>
          <input type="number" id="poll_min" name="poll_min" min="2" max="60" value=")rawliteral" + String(cfg.pollMinutes) + R"rawliteral(">
        </div>
      </div>

      <div class="row">
        <div>
          <label for="bright">LED Helligkeit (10-255)</label>
          <input type="number" id="bright" name="bright" min="10" max="255" value=")rawliteral" + String(cfg.brightness) + R"rawliteral(">
        </div>
        <div>
          <label for="cs_marq">Codeshare-Anzeige</label>
          <select id="cs_marq" name="cs_marq">
            <option value="0" )rawliteral" + String(!cfg.codeshareMarquee ? "selected" : "") + R"rawliteral(>Nur fliegende Airline</option>
            <option value="1" )rawliteral" + String(cfg.codeshareMarquee ? "selected" : "") + R"rawliteral(>Laufschrift (Alle Nummern)</option>
          </select>
        </div>
      </div>

      <button type="submit" class="btn">Speichern &amp; Anwenden</button>
    </form>

    <form method="POST" action="/switch">
      <button type="submit" class="btn btn-sec">⇄ Jetzt zwischen Abflug &amp; Ankunft wechseln</button>
    </form>
    <form method="GET" action="/">
      <button type="submit" class="btn btn-sec">↻ WLAN-Liste neu scannen</button>
    </form>

    <div class="status">Status: )rawliteral" + htmlEscape(lastApiStatus) + " | IP: " + WiFi.localIP().toString() + R"rawliteral(</div>
  </div>
</body>
</html>)rawliteral";

  return html;
}

String scanWifiDropdownOptions() {
  int n = WiFi.scanNetworks();
  String opts = "";
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    String sel = (s == cfg.wifiSsid) ? " selected" : "";
    String esc = htmlEscape(s);
    opts += "<option value=\"" + esc + "\"" + sel + ">" + esc + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();
  return opts;
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    String options = scanWifiDropdownOptions();
    server.send(200, "text/html", buildHtmlPage(options));
  });

  server.on("/switch", HTTP_POST, []() {
    showDepartures = !showDepartures;
    lastSwitchMs = millis();
    animFrame = 0;
    renderMatrixDisplay();
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/save", HTTP_POST, []() {
    bool needsRestart = false;
    if (server.hasArg("ssid") && server.arg("ssid").length() > 0) {
      String newSsid = server.arg("ssid");
      if (newSsid != cfg.wifiSsid) {
        cfg.wifiSsid = newSsid;
        needsRestart = true;
      }
    }
    if (server.hasArg("pass") && server.arg("pass").length() > 0) {
      cfg.wifiPass = server.arg("pass");
      needsRestart = true;
    }
    if (server.hasArg("api_key") && server.arg("api_key").length() > 0) {
      cfg.apiKey = server.arg("api_key");
      cfg.apiKey.trim();
    }
    if (server.hasArg("iata") && server.arg("iata").length() == 3) {
      String iata = server.arg("iata");
      iata.toUpperCase();
      if (isAlpha(iata[0]) && isAlpha(iata[1]) && isAlpha(iata[2])) cfg.iataCode = iata;
    }
    if (server.hasArg("height")) {
      // Die Panel-Anordnung wird beim Start festgelegt -> Neustart noetig
      int newHeight = server.arg("height").toInt() == 64 ? 64 : 128;
      if (newHeight != cfg.matrixHeight) {
        cfg.matrixHeight = newHeight;
        needsRestart = true;
      }
    }
    if (server.hasArg("sw_sec")) cfg.switchSeconds = constrain(server.arg("sw_sec").toInt(), 4, 120);
    if (server.hasArg("poll_min")) cfg.pollMinutes = constrain(server.arg("poll_min").toInt(), 2, 60);
    if (server.hasArg("bright")) {
      cfg.brightness = constrain(server.arg("bright").toInt(), 10, 255);
      if (dma_display) dma_display->setBrightness8(cfg.brightness);
    }
    if (server.hasArg("cs_marq")) cfg.codeshareMarquee = (server.arg("cs_marq") == "1");

    saveConfig();

    if (needsRestart) {
      server.send(200, "text/html", buildHtmlPage("", "Einstellungen gespeichert! ESP32 startet neu..."));
      delay(1500);
      ESP.restart();
    } else {
      updateAllFlightData();
      renderMatrixDisplay();
      String options = scanWifiDropdownOptions();
      server.send(200, "text/html", buildHtmlPage(options, "Einstellungen erfolgreich gespeichert und aktualisiert!"));
    }
  });

  server.begin();
}

// ============================================================================
// 11. WLAN VERBINDUNG & SETUP
// ============================================================================
void connectWifiOrStartAP() {
  WiFi.mode(WIFI_STA);
  if (cfg.wifiSsid.length() > 0) {
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
      delay(300);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    apModeActive = false;
    MDNS.begin("airportboard"); // Erreichbar unter http://airportboard.local
    configTzTime(cfg.timezone.c_str(), "pool.ntp.org", "time.nist.gov");
  } else {
    // Fallback Access Point fuer Ersteinrichtung
    apModeActive = true;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("AIRPORT-BOARD-SETUP");
  }
}

void setup() {
  Serial.begin(115200);
  loadConfig();

  // HUB75 / HUB75E Initialisierung: 1 Reihe (256x64) oder 2 Reihen (256x128) aus 64x64-Panels
  const int panelRows = cfg.matrixHeight / BOARD_PANEL_RES_Y;
  HUB75_I2S_CFG mxconfig(BOARD_PANEL_RES_X, BOARD_PANEL_RES_Y, panelRows * BOARD_PANEL_COLS);
  mxconfig.gpio.r1 = BOARD_R1_PIN;
  mxconfig.gpio.g1 = BOARD_G1_PIN;
  mxconfig.gpio.b1 = BOARD_B1_PIN;
  mxconfig.gpio.r2 = BOARD_R2_PIN;
  mxconfig.gpio.g2 = BOARD_G2_PIN;
  mxconfig.gpio.b2 = BOARD_B2_PIN;
  mxconfig.gpio.a = BOARD_A_PIN;
  mxconfig.gpio.b = BOARD_B_PIN;
  mxconfig.gpio.c = BOARD_C_PIN;
  mxconfig.gpio.d = BOARD_D_PIN;
  mxconfig.gpio.e = BOARD_E_PIN;
  mxconfig.gpio.lat = BOARD_LAT_PIN;
  mxconfig.gpio.oe = BOARD_OE_PIN;
  mxconfig.gpio.clk = BOARD_CLK_PIN;
  mxconfig.clkphase = false;
  mxconfig.double_buff = BOARD_DOUBLE_BUFFER;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  if (!dma_display->begin()) {
    Serial.println("HUB75 DMA Init fehlgeschlagen (zu wenig RAM?)");
  }
  dma_display->setBrightness8(cfg.brightness);
  dma_display->clearScreen();

  gfx = new BoardDisplay(*dma_display, panelRows, BOARD_PANEL_COLS, BOARD_PANEL_RES_X, BOARD_PANEL_RES_Y);

  // Farben initialisieren
  COL_BG = gfx->color565(0, 0, 0);
  COL_HEADER_BG = gfx->color565(22, 16, 4);
  COL_AMBER = gfx->color565(255, 190, 11);
  COL_WHITE = gfx->color565(255, 255, 255);
  COL_GREEN = gfx->color565(40, 255, 30);
  COL_RED = gfx->color565(255, 45, 45);
  COL_CYAN = gfx->color565(0, 225, 255);
  COL_BELT = gfx->color565(125, 210, 255);
  COL_DIM = gfx->color565(150, 150, 160);
  COL_LINE = gfx->color565(28, 24, 16);

  loadDemoData();
  renderMatrixDisplay();

  connectWifiOrStartAP();
  setupWebServer();

  if (!apModeActive) {
    updateAllFlightData();
  }
  renderMatrixDisplay();
}

void loop() {
  server.handleClient();

  unsigned long nowMs = millis();

  // Automatischer Wechsel zwischen DEPARTURES und ARRIVALS
  if (!apModeActive && (nowMs - lastSwitchMs >= (unsigned long)cfg.switchSeconds * 1000UL)) {
    lastSwitchMs = nowMs;
    showDepartures = !showDepartures;
    animFrame = 0;
    renderMatrixDisplay();
  }

  // Regelmaessiger AirLabs API Abruf
  if (!apModeActive && (nowMs - lastPollMs >= (unsigned long)cfg.pollMinutes * 60000UL)) {
    lastPollMs = nowMs;
    updateAllFlightData();
    renderMatrixDisplay();
  }

  // Fluessige Animation fuer lange Ortsnamen (PALMA DE MALLORCA) & Codeshare-Laufschrift;
  // sonst nur neu zeichnen, wenn sich die Uhrzeit aendert (weniger Flackern)
  if (nowMs - lastAnimMs >= 90) {
    lastAnimMs = nowMs;
    String clk = apModeActive ? String("") : getCurrentClockHHMM();
    if (animNeeded || clk != lastClockStr) {
      lastClockStr = clk;
      if (animNeeded) animFrame++;
      renderMatrixDisplay();
    }
  }
}
