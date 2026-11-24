/*
  ============================================================================
  ESP32 HUB75 / HUB75E LED MATRIX AIRPORT DEPARTURE & ARRIVAL BOARD (HAM)
  ============================================================================
  Hardware
   - ESP32 (esp32dev)
   - 2x HUB75E LED Matrix Panels 128x64 nebeneinander = 256x64 (3 Flugzeilen)

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
#define BOARD_PANEL_RES_X 128 // Breite eines einzelnen Panels
#define BOARD_PANEL_RES_Y 64  // Hoehe eines einzelnen Panels
#define BOARD_PANEL_COLS 2    // 2 Panels nebeneinander
#define BOARD_PANEL_ROWS 1
#define BOARD_SCREEN_W (BOARD_PANEL_RES_X * BOARD_PANEL_COLS)  // 256
#define BOARD_SCREEN_H (BOARD_PANEL_RES_Y * BOARD_PANEL_ROWS)  // 64

// Bei einer Reihe ohne Bedeutung, wird nur vom VirtualMatrixPanel gebraucht
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
  time_t eventTs = 0; // Zeitpunkt von Start/Landung/Annullierung (Basis fuer die Anzeigedauer)
  uint8_t kind = 0;   // 0 = normal, 1 = gestartet/gelandet, 2 = annulliert
};

// Anzeigedauer nach dem Ereignis in Sekunden
#define SHOW_DEPARTED_SEC 300   // gestartet: 5 min
#define SHOW_LANDED_SEC 600     // gelandet: 10 min
#define SHOW_CANCELLED_SEC 3600 // annulliert: 1 h
#define MAX_FINISHED_ROWS 2     // gestartet/gelandet: hoechstens 2 Zeilen
#define DELAYED_AFTER_SEC 900   // Abflug nicht gestartet, 15 min nach Plan: DELAYED
#define STALE_DEPARTURE_SEC 7200 // nicht aktualisierte Abfluege nach 2 h ausblenden

// Lokale Zeit eines AirLabs-Textes "YYYY-MM-DD HH:MM" (Flughafenzeit = eingestellte Zeitzone) als Unix-Zeit
time_t parseLocalTs(const char *s) {
  int y, mo, d, h, mi;
  if (!s || sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_isdst = -1;
  return mktime(&t);
}

bool isTimeValid() {
  return time(nullptr) > 1700000000; // vor der NTP-Synchronisation liegt die Uhr bei 1970
}

bool isEntryExpired(const FlightEntry &e, time_t now) {
  if (e.kind == 0 || e.eventTs <= 0) return false;
  long limit = e.kind == 2 ? SHOW_CANCELLED_SEC : (e.statusLabel == "DEPARTED" ? SHOW_DEPARTED_SEC : SHOW_LANDED_SEC);
  return (now - e.eventTs) > limit;
}

#define MAX_FLIGHTS 12
FlightEntry depFlights[MAX_FLIGHTS];
int depCount = 0;

FlightEntry arrFlights[MAX_FLIGHTS];
int arrCount = 0;

bool showDepartures = true;
bool isLiveApiData = false;
bool apModeActive = false;
bool animNeeded = false; // wird beim Rendern gesetzt, wenn ein Text scrollen muss
bool refreshRequested = false; // vom Web-Interface gesetzt, loop() ruft dann die API ab
String lastApiStatus = "Initialisiere...";
unsigned long infoUntilMs = 0; // solange millis() darunter liegt, zeigt das Display den Verbindungs-Infoschirm
String lastClockStr = "";
static const char *AP_SSID = "AIRPORT-BOARD-SETUP";
String apIpStr = "192.168.4.1";

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
  cfg.matrixHeight = BOARD_SCREEN_H; // feste Hardware: 256x64
  cfg.switchSeconds = prefs.getInt("sw_sec", 15);
  cfg.pollMinutes = prefs.getInt("poll_min", 10);
  cfg.brightness = prefs.getInt("bright", 90);
  cfg.codeshareMarquee = prefs.getBool("cs_marq", false);
  cfg.timezone = prefs.getString("tz", "CET-1CEST,M3.5.0,M10.5.0/3");
  prefs.end();

}

void saveConfig() {
  prefs.begin("airport_cfg", false);
  prefs.putString("ssid", cfg.wifiSsid);
  prefs.putString("pass", cfg.wifiPass);
  prefs.putString("api_key", cfg.apiKey);
  prefs.putString("iata", cfg.iataCode);
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
// Stream-Adapter fuer ArduinoJson: wartet auf Daten, solange die TLS-Verbindung offen ist.
// Client::read() liefert sonst bei kurzen Luecken im Datenstrom -1, was der Parser als Ende wertet.
class WaitingStream : public Stream {
 public:
  explicit WaitingStream(Client &c) : client_(c) {}
  uint32_t bytesRead = 0;

  int available() override { return client_.available(); }
  int read() override {
    if (!waitForData()) return -1;
    bytesRead++;
    return client_.read();
  }
  int peek() override { return waitForData() ? client_.peek() : -1; }
  size_t readBytes(char *buffer, size_t length) override {
    size_t n = 0;
    while (n < length) {
      int c = read();
      if (c < 0) break;
      buffer[n++] = (char)c;
    }
    return n;
  }
  size_t write(uint8_t) override { return 0; }
  void flush() override {}

 private:
  bool waitForData() {
    unsigned long start = millis();
    while (client_.available() == 0) {
      if (!client_.connected() || millis() - start > 10000) return false;
      delay(1);
    }
    return true;
  }
  Client &client_;
};

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
  // _fields: nur die benoetigten Felder abrufen (Antwort wird ca. 3x kleiner, schont RAM und TLS-Verbindung)
  String url = "https://airlabs.co/api/v9/schedules?" + param + cfg.iataCode + "&api_key=" + key +
               "&_fields=flight_iata,cs_flight_iata,dep_iata,arr_iata,dep_gate,arr_gate,arr_baggage,dep_time,arr_time,"
               "dep_estimated,arr_estimated,dep_actual,arr_actual,dep_time_ts,arr_time_ts,dep_actual_ts,arr_actual_ts,status,delayed,dep_delayed,arr_delayed";

  if (!http.begin(client, url)) {
    lastApiStatus = "HTTPS Verbindung fehlgeschlagen";
    return false;
  }

  unsigned long t0 = millis();
  Serial.printf("AirLabs %s: Abruf startet, Heap frei %u, groesster Block %u\n", isDeparture ? "dep" : "arr", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    char tlsErr[80] = "";
    client.lastError(tlsErr, sizeof(tlsErr));
    lastApiStatus = "HTTP Fehler " + String(httpCode) + " (" + HTTPClient::errorToString(httpCode) + ")";
    Serial.printf("AirLabs %s: %s | TLS: %s | Heap frei %u, groesster Block %u\n", isDeparture ? "dep" : "arr", lastApiStatus.c_str(), tlsErr,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
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
  fObj["dep_actual"] = true;
  fObj["arr_actual"] = true;
  fObj["arr_actual_ts"] = true;
  fObj["status"] = true;
  fObj["delayed"] = true;
  fObj["dep_delayed"] = true;
  fObj["arr_delayed"] = true;
  filter["error"]["message"] = true;

#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument doc;
#else
  DynamicJsonDocument doc(24576);
#endif
  // Kleine Antworten (z.B. Fehlermeldungen) komplett lesen, grosse Flugplaene als Stream parsen
  DeserializationError err;
  int bodyLen = http.getSize();
  uint32_t streamedBytes = 0;
  if (bodyLen > 0 && bodyLen < 4096) {
    String body = http.getString();
    err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  } else {
    WaitingStream body(http.getStream());
    err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    streamedBytes = body.bytesRead;
  }
  char tlsErr[80] = "";
  client.lastError(tlsErr, sizeof(tlsErr));
  http.end();
  Serial.printf("AirLabs %s: HTTP %d, Content-Length %d, gelesen %u in %lu ms, JSON: %s, TLS: %s\n", isDeparture ? "dep" : "arr", httpCode, bodyLen,
                (unsigned)streamedBytes, millis() - t0, err.c_str(), tlsErr);

  const char *apiError = doc["error"]["message"] | static_cast<const char *>(nullptr);
  if (apiError) {
    lastApiStatus = String("AirLabs: ") + apiError;
    return false;
  }

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
  const bool timeValid = isTimeValid();
  int itemsTotal = 0;
  targetCount = 0;

  for (JsonObject item : arr) {
    itemsTotal++;
    const char *fIataRaw = item["flight_iata"] | "";
    String fIata(fIataRaw);
    fIata.trim();
    if (fIata.length() == 0) continue;

    String status(item["status"] | "scheduled");
    status.toLowerCase();

    time_t schedTs = isDeparture ? (item["dep_time_ts"] | 0L) : (item["arr_time_ts"] | 0L);
    time_t actualTs = isDeparture ? (item["dep_actual_ts"] | 0L) : (item["arr_actual_ts"] | 0L);
    // Fehlen Unix-Zeitstempel in der Antwort, werden sie aus den lokalen Zeittexten berechnet
    if (schedTs == 0) schedTs = parseLocalTs(item[isDeparture ? "dep_time" : "arr_time"].as<const char *>());
    if (actualTs == 0) actualTs = parseLocalTs(item[isDeparture ? "dep_actual" : "arr_actual"].as<const char *>());
    const time_t estTs = parseLocalTs(item[isDeparture ? "dep_estimated" : "arr_estimated"].as<const char *>());
    const bool cancelled = status == "cancelled";
    const bool done = !cancelled && (isDeparture ? (status == "active" || status == "landed" || actualTs > 0) : status == "landed");
    const time_t eventTs = actualTs > 0 ? actualTs : (estTs > 0 ? estTs : schedTs);

    if (timeValid) {
      // Gestartet/gelandet/annulliert nur begrenzte Zeit nach dem Ereignis anzeigen
      if (cancelled && eventTs > 0 && (now - eventTs > SHOW_CANCELLED_SEC)) continue;
      if (done && eventTs > 0 && (now - eventTs > (isDeparture ? SHOW_DEPARTED_SEC : SHOW_LANDED_SEC))) continue;
      // Verspaetete, nicht aktualisierte Fluege nicht ewig stehen lassen
      if (!cancelled && !done && schedTs > 0) {
        if (isDeparture && (now - schedTs > STALE_DEPARTURE_SEC) && status == "scheduled") continue;
        if (!isDeparture && (now - schedTs > 2700) && status != "active") continue;
      }
    }

    String remoteIata = isDeparture ? String(item["arr_iata"] | "") : String(item["dep_iata"] | "");
    String city = resolveCityName(remoteIata);
    String schedHHMM = isDeparture ? extractHHMM(item["dep_time"].as<const char *>()) : extractHHMM(item["arr_time"].as<const char *>());
    const char *csRaw = item["cs_flight_iata"].as<const char *>();
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
      const char *g = item["dep_gate"].as<const char *>();
      entry.gateOrBelt = (g && strlen(g) > 0) ? String(g) : "-";
      entry.isBaggageBelt = false;
    } else {
      const char *b = item["arr_baggage"].as<const char *>();
      const char *g = item["arr_gate"].as<const char *>();
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
    String estHHMM = isDeparture ? extractHHMM(item["dep_estimated"].as<const char *>()) : extractHHMM(item["arr_estimated"].as<const char *>());

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
    } else if (isDeparture && done) {
      entry.statusLabel = "DEPARTED";
      entry.statusColor = COL_CYAN;
    } else if (delayMin > 5 && estHHMM != "--:--") {
      entry.statusLabel = "EST " + estHHMM;
      entry.statusColor = COL_AMBER;
    } else {
      entry.statusLabel = "ON TIME";
      entry.statusColor = COL_GREEN;
    }

    entry.kind = cancelled ? 2 : (done ? 1 : 0);
    entry.eventTs = eventTs;

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

  for (int i = 0; i < targetCount; i++) {
    const FlightEntry &e = targetArr[i];
    Serial.printf("  %s %s %-10s %-9s kind=%d sched=%ld event=%ld\n", e.schedTime.c_str(), e.operatingFlight.c_str(), e.cityName.c_str(),
                  e.statusLabel.c_str(), e.kind, (long)e.sortTs, (long)e.eventTs);
  }
  Serial.printf("AirLabs %s: %d Fluege empfangen, %d angezeigt, Uhrzeit gueltig=%d\n", isDeparture ? "dep" : "arr", itemsTotal, targetCount, timeValid);
  lastApiStatus = "Live OK (" + cfg.iataCode + ")";
  return true;
}

// Erkennt gestartete Abfluege ueber die Echtzeit-Flugdaten (/flights = Flugzeuge in der Luft).
// Der Flugplan meldet "scheduled", obwohl die Maschine laengst abgehoben hat.
struct SeenDeparture {
  String flight;
  time_t ts;
};
SeenDeparture seenDepartures[MAX_FLIGHTS];
int seenDepartureNext = 0;

void markAirborneDepartures() {
  String key = cfg.apiKey;
  key.trim();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);
  String url = "https://airlabs.co/api/v9/flights?dep_iata=" + cfg.iataCode + "&api_key=" + key + "&_fields=flight_iata,cs_flight_iata";
  if (!http.begin(client, url) || http.GET() != HTTP_CODE_OK) {
    Serial.println("AirLabs flights: Abruf fehlgeschlagen, Start-Erkennung uebersprungen");
    http.end();
    return;
  }

#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument filter;
  JsonDocument doc;
#else
  StaticJsonDocument<128> filter;
  DynamicJsonDocument doc(8192);
#endif
  JsonObject fObj = filter["response"][0].to<JsonObject>();
  fObj["flight_iata"] = true;
  fObj["cs_flight_iata"] = true;
  WaitingStream body(http.getStream());
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("AirLabs flights: JSON %s\n", err.c_str());
    return;
  }

  // Flugnummern der Maschinen in der Luft als ",LH123,AB45," ablegen
  String airborne = ",";
  for (JsonObject f : doc["response"].as<JsonArray>()) {
    const char *a = f["flight_iata"].as<const char *>();
    const char *b = f["cs_flight_iata"].as<const char *>();
    if (a && *a) airborne += String(a) + ",";
    if (b && *b) airborne += String(b) + ",";
  }

  const time_t now = time(nullptr);
  int marked = 0;
  for (int i = 0; i < depCount; i++) {
    FlightEntry &e = depFlights[i];
    if (e.kind != 0) continue;
    if (airborne.indexOf("," + e.operatingFlight + ",") < 0) {
      // Auch Codeshare-Nummern pruefen (allFlights = "LH1  OS2  LX3")
      bool hit = false;
      int start = 0;
      while (start < (int)e.allFlights.length() && !hit) {
        int sp = e.allFlights.indexOf(' ', start);
        String code = e.allFlights.substring(start, sp < 0 ? e.allFlights.length() : sp);
        if (code.length() > 0 && airborne.indexOf("," + code + ",") >= 0) hit = true;
        start = sp < 0 ? e.allFlights.length() : sp + 1;
      }
      if (!hit) continue;
    }

    // Erste Erkennung merken, damit die 5-Minuten-Anzeigedauer nicht bei jedem Abruf neu beginnt
    time_t ts = 0;
    for (int s = 0; s < MAX_FLIGHTS; s++) {
      if (seenDepartures[s].flight == e.operatingFlight && seenDepartures[s].ts > 0) ts = seenDepartures[s].ts;
    }
    if (ts == 0) {
      ts = (e.sortTs > 0 && now > e.sortTs + 900) ? e.sortTs + 900 : now;
      seenDepartures[seenDepartureNext] = {e.operatingFlight, ts};
      seenDepartureNext = (seenDepartureNext + 1) % MAX_FLIGHTS;
    }
    e.kind = 1;
    e.eventTs = ts;
    e.statusLabel = "DEPARTED";
    e.statusColor = COL_CYAN;
    marked++;
  }
  Serial.printf("AirLabs flights: %d Maschinen in der Luft, %d Abfluege als gestartet erkannt\n", (int)doc["response"].size(), marked);
}

void updateAllFlightData() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!isLiveApiData) loadDemoData();
    return;
  }

  // Ohne synchronisierte Uhr greifen die Zeitregeln nicht -> kurz auf NTP warten
  for (int i = 0; i < 50 && !isTimeValid(); i++) delay(200);
  if (!isTimeValid()) Serial.println("API: Uhrzeit nicht synchronisiert, Zeitfilter inaktiv");

  // Direkt nach dem WLAN-Start schlagen TLS-Verbindungen gelegentlich fehl -> bis zu 3 Versuche je Abruf
  bool okDep = false;
  bool okArr = false;
  for (int attempt = 1; attempt <= 3 && !(okDep && okArr); attempt++) {
    if (attempt > 1) {
      Serial.printf("API: Wiederholung %d (Abflug ok=%d, Ankunft ok=%d)\n", attempt, okDep, okArr);
      delay(2000);
    }
    if (!okDep) okDep = fetchAirLabsEndpoint(true, depFlights, depCount);
    if (!okArr) okArr = fetchAirLabsEndpoint(false, arrFlights, arrCount);
  }

  if (okDep) markAirborneDepartures();

  if (okDep && okArr) {
    isLiveApiData = true;
    lastApiStatus = "Live OK (" + cfg.iataCode + ")";
  } else {
    if (okDep != okArr) lastApiStatus = String(okDep ? "Ankunft: " : "Abflug: ") + lastApiStatus;
    // Bei kurzzeitigem Fehler bleiben bereits geladene Live-Daten stehen
    if (!isLiveApiData) loadDemoData();
  }
  Serial.printf("API: %s | live=%d | dep=%d arr=%d | Heap frei %u\n", lastApiStatus.c_str(), isLiveApiData, depCount, arrCount, ESP.getFreeHeap());
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

  // Auf dem Infoschirm "CONNECTED" immer das startende Flugzeug zeigen
  drawPlaneIcon(3, 3, showDepartures || (!apModeActive && millis() < infoUntilMs), COL_AMBER);

  gfx->setTextColor(COL_AMBER);
  gfx->setCursor(20, 5);
  gfx->print(apModeActive ? "WIFI SETUP" : (millis() < infoUntilMs ? "CONNECTED" : (showDepartures ? "DEPARTURES" : "ARRIVALS")));

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

  // Infoschirm direkt nach dem Verbinden: zeigt, unter welcher Adresse die Einstellungen erreichbar sind
  if (!apModeActive && millis() < infoUntilMs) {
    const bool tall = cfg.matrixHeight >= 128;
    const int lineH = tall ? 18 : 11;
    int y = tall ? 26 : 22;
    const int valX = 44;
    String ip = WiFi.localIP().toString();
    const char *labels[4] = {"WIFI", "IP", "WEB", "API"};
    String values[4] = {cfg.wifiSsid, ip, "http://" + ip, lastApiStatus};
    uint16_t colors[4] = {COL_CYAN, COL_GREEN, COL_GREEN, isLiveApiData ? COL_GREEN : COL_RED};
    for (int i = 0; i < 4; i++) {
      gfx->setTextColor(COL_AMBER);
      gfx->setCursor(4, y);
      gfx->print(labels[i]);
      gfx->setTextColor(colors[i]);
      gfx->setCursor(valX, y);
      gfx->print(values[i].substring(0, (BOARD_SCREEN_W - valX) / 6));
      y += lineH;
    }
    return;
  }

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
      gfx->print(AP_SSID);
      gfx->setTextColor(COL_GREEN);
      gfx->setCursor(52 + (int)strlen(AP_SSID) * 6 + 12, 58);
      gfx->print(apIpStr);
      gfx->drawFastHLine(0, 71, BOARD_SCREEN_W, COL_LINE);

      gfx->setTextColor(COL_AMBER);
      gfx->setCursor(4, 78);
      gfx->print("WEB");
      gfx->setTextColor(COL_GREEN);
      gfx->setCursor(52, 78);
      gfx->print("http://" + apIpStr);
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
      gfx->print(AP_SSID);
      gfx->setTextColor(COL_GREEN);
      gfx->setCursor(40 + (int)strlen(AP_SSID) * 6 + 12, 38);
      gfx->print(apIpStr);
      gfx->drawFastHLine(0, 49, BOARD_SCREEN_W, COL_LINE);

      gfx->setTextColor(COL_AMBER);
      gfx->setCursor(4, 53);
      gfx->print("URL");
      gfx->setTextColor(COL_GREEN);
      gfx->setCursor(40, 53);
      gfx->print("http://" + apIpStr);
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

  // Abgelaufene Fluege (Zeitregeln siehe SHOW_*_SEC) ausblenden; gestartet/gelandet belegen
  // hoechstens MAX_FINISHED_ROWS Zeilen (die juengsten), den Rest fuellen anstehende Fluege.
  const bool timeValid = isTimeValid();
  const time_t nowTs = time(nullptr);
  auto isHidden = [&](const FlightEntry &e) { return timeValid && isEntryExpired(e, nowTs); };

  int finishedTotal = 0;
  for (int i = 0; i < count; i++) {
    if (!isHidden(list[i]) && list[i].kind == 1) finishedTotal++;
  }
  int finishedToSkip = finishedTotal > MAX_FINISHED_ROWS ? finishedTotal - MAX_FINISHED_ROWS : 0;

  int shown[7];
  int visible = 0;
  for (int i = 0; i < count && visible < maxRows; i++) {
    if (isHidden(list[i])) continue;
    if (list[i].kind == 1 && finishedToSkip > 0) {
      finishedToSkip--;
      continue;
    }
    shown[visible++] = i;
  }

  if (visible == 0) {
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(64, startY + 14);
    gfx->print("NO FLIGHTS SCHEDULED");
    return;
  }

  for (int i = 0; i < visible; i++) {
    int y = startY + (i * rowHeight);
    int textY = y + 3;
    FlightEntry &f = list[shown[i]];

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
    // Abflug-Gates haben dieselbe Farbe wie die Gepaeckbaender der Ankunftstafel
    gfx->setTextColor((showDepartures || f.isBaggageBelt) ? COL_BELT : COL_CYAN);
    gfx->setCursor(164, textY);
    gfx->print(f.gateOrBelt);

    // STATUS (rechtsbuendig bis x=255, genuegend Platz fuer 9 Zeichen wie CANCELLED)
    String statusText = f.statusLabel;
    uint16_t statusCol = f.statusColor;
    // Abflug nicht gestartet und 15 min nach der geplanten Zeit: DELAYED (ersetzt ON TIME/EST)
    if (showDepartures && f.kind == 0 && timeValid && f.sortTs > 0 && (nowTs - f.sortTs) > DELAYED_AFTER_SEC) {
      statusText = "DELAYED";
      statusCol = COL_AMBER;
    }
    int stWidth = (int)statusText.length() * 6;
    int stX = 255 - stWidth;
    if (stX < 192) stX = 192;
    gfx->setTextColor(statusCol);
    gfx->setCursor(stX, textY);
    gfx->print(statusText);

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
          <label for="sw_sec">Wechsel Abflug/Ankunft (Sek.)</label>
          <input type="number" id="sw_sec" name="sw_sec" min="4" max="120" value=")rawliteral" + String(cfg.switchSeconds) + R"rawliteral(">
        </div>
      </div>

      <div class="row">
        <div>
          <label for="poll_min">API Update (Min.)</label>
          <input type="number" id="poll_min" name="poll_min" min="2" max="60" value=")rawliteral" + String(cfg.pollMinutes) + R"rawliteral(">
        </div>
        <div>
          <label for="bright">LED Helligkeit (10-255)</label>
          <input type="number" id="bright" name="bright" min="10" max="255" value=")rawliteral" + String(cfg.brightness) + R"rawliteral(">
        </div>
      </div>

      <div class="row">
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
      // Abruf erst nach der Antwort in loop() ausfuehren: im Request ist der Heap fuer TLS oft zu knapp/fragmentiert
      refreshRequested = true;
      String options = scanWifiDropdownOptions();
      server.send(200, "text/html",
                  buildHtmlPage(options, "Einstellungen gespeichert. Flugdaten werden jetzt neu geladen - Seite in ca. 20 Sekunden neu laden, dann zeigt der Status unten das Ergebnis."));
    }
  });

  server.begin();
}

// ============================================================================
// 11. WLAN VERBINDUNG & SETUP
// ============================================================================
void connectWifiOrStartAP() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Modem-Sleep macht TLS-Verbindungen unzuverlaessig
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
    MDNS.addService("http", "tcp", 80);
    Serial.printf("WLAN verbunden: %s  IP: %s\n", cfg.wifiSsid.c_str(), WiFi.localIP().toString().c_str());
    configTzTime(cfg.timezone.c_str(), "pool.ntp.org", "time.nist.gov");
  } else {
    // Fallback Access Point fuer Ersteinrichtung
    apModeActive = true;
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(AP_SSID)) {
      Serial.println("SoftAP konnte nicht gestartet werden (zu wenig RAM?)");
    }
    apIpStr = WiFi.softAPIP().toString();
    Serial.printf("Setup-WLAN: %s  IP: %s\n", AP_SSID, apIpStr.c_str());
  }
}

void setup() {
  Serial.begin(115200);
  Serial.printf("\nStart, Reset-Grund: %d (1=Power-on 3=Software 4=Panic 5/6/7=Watchdog 9=Brownout)\n", (int)esp_reset_reason());
  loadConfig();

  // WLAN-Treiber zuerst initialisieren: Der DMA-Puffer der Matrix belegt sonst fast den
  // gesamten internen RAM und esp_wifi_init scheitert mit ESP_ERR_NO_MEM
  WiFi.mode(WIFI_STA);
  Serial.printf("Freier Heap nach WLAN-Init: %u (groesster Block %u)\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // HUB75 / HUB75E Initialisierung: 2 Panels 128x64 nebeneinander = 256x64
  HUB75_I2S_CFG mxconfig(BOARD_PANEL_RES_X, BOARD_PANEL_RES_Y, BOARD_PANEL_COLS);
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

  Serial.println("Display-Init startet...");
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  if (!dma_display->begin()) {
    Serial.println("HUB75 DMA Init fehlgeschlagen (zu wenig RAM?)");
  }
  dma_display->setBrightness8(cfg.brightness);
  dma_display->clearScreen();
  Serial.printf("Freier Heap nach Display-Init: %u (groesster Block %u)\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  gfx = new BoardDisplay(*dma_display, BOARD_PANEL_ROWS, BOARD_PANEL_COLS, BOARD_PANEL_RES_X, BOARD_PANEL_RES_Y);

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
    infoUntilMs = millis() + 20000UL;
  }
  renderMatrixDisplay();
}

void loop() {
  server.handleClient();

  unsigned long nowMs = millis();

  if (infoUntilMs != 0 && nowMs >= infoUntilMs) {
    infoUntilMs = 0;
    lastSwitchMs = nowMs;
    renderMatrixDisplay();
  }
  const bool infoActive = nowMs < infoUntilMs;

  // Automatischer Wechsel zwischen DEPARTURES und ARRIVALS
  if (!infoActive && !apModeActive && (nowMs - lastSwitchMs >= (unsigned long)cfg.switchSeconds * 1000UL)) {
    lastSwitchMs = nowMs;
    showDepartures = !showDepartures;
    animFrame = 0;
    renderMatrixDisplay();
  }

  // Regelmaessiger AirLabs API Abruf
  if (!apModeActive && (refreshRequested || nowMs - lastPollMs >= (unsigned long)cfg.pollMinutes * 60000UL)) {
    refreshRequested = false;
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
