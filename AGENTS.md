# AGENTS.md — Projektdokumentation für KI-Agenten

> Diese Datei beschreibt das Projekt vollständig für KI-Assistenten (Copilot, Cursor, Claude, Gemini, etc.),
> damit diese den Kontext, die Architektur und die Einschränkungen kennen, bevor sie Code ändern.

---

## Projektübersicht

**ESP32 HUB75 Airport Departure & Arrival Matrix Board**

Ein ESP32-basiertes Echtzeit-Anzeigesystem, das Abflüge und Ankünfte eines beliebigen Flughafens
auf einer kaskadierenden HUB75E-LED-Matrix anzeigt. Die Flugdaten kommen live von der
[AirLabs Schedules-API](https://airlabs.co/docs/schedules). Ohne API-Key oder WLAN läuft ein eingebauter Demo-Modus.

---

## Hardware

### Aktueller Aufbau (implementiert)
| Komponente | Details |
|---|---|
| Mikrocontroller | ESP32 (`esp32dev`) |
| Displays | **4× HUB75E LED-Matrix-Panel, je 128×64 Pixel** |
| Anordnung | **2 Spalten × 2 Reihen**, U-Kette (`CHAIN_TOP_LEFT_DOWN`): oben links→rechts aufrecht, unten kopfüber rechts→links |
| Gesamtauflösung | **256×128 Pixel** |
| Sichtbare Flugzeilen | 7 Einträge gleichzeitig |

> **Wichtig für KI-Agenten:** `cfg.matrixHeight` kommt aus `BOARD_SCREEN_H` (128). Damit sind Spaltenköpfe
> (TIME / FLIGHT / DEST oder FROM / GATE oder BELT / STATUS), 7 Flugzeilen und das hohe Setup-Layout aktiv.
> Die DMA-Kette hat `BOARD_PANEL_CHAIN` = 4 Panels, das Virtual-Grid ist 2 Zeilen × 2 Spalten.

### Pin-Belegung (HUB75E — 64px-hohe Panels benötigen E-Pin!)
| Signal | GPIO |
|---|---|
| R1 | 25 |
| G1 | 26 |
| B1 | 27 |
| R2 | 14 |
| G2 | 12 |
| B2 | 13 |
| A  | 23 |
| B  | 19 |
| C  | 5  |
| D  | 17 |
| **E** | **18** ← für HUB75**E** zwingend! |
| LAT | 4 |
| OE  | 15 |
| CLK | 16 |

---

## Software-Architektur

Alle Firmware befindet sich in einer einzigen Datei: [`src/main.cpp`](src/main.cpp).

### Abhängigkeiten (`platformio.ini`)
| Bibliothek | Version | Zweck |
|---|---|---|
| `ESP32 HUB75 LED MATRIX PANEL DMA Display` (mrfaptastic) | ^3.1.1 | DMA-basiertes Panel-Driving |
| `Adafruit GFX Library` | ^1.12.5 | Textrendering, Grafikprimitiven |
| `ArduinoJson` | ^7.0.0 | JSON-Parsing der API-Antworten |

**Build-Flag:** `-DPIXEL_COLOR_DEPTH_BITS=4`  
Grund: HTTPS/TLS benötigt ~40 KB zusammenhängenden Heap. 4-Bit-Farbtiefe gibt dem DMA-Puffer weniger RAM;
6 Bit würden zu wenig für TLS-Verbindungen lassen.

### Modulstruktur (Kapitelgliederung in `main.cpp`)
| Abschnitt | Inhalt |
|---|---|
| §1 | AirLabs API-Key (Standardwert aus `secrets.h`, überschreibbar im Web-Interface) |
| §2 | HUB75/HUB75E Panel-Konfiguration (Auflösung, Pins, Double-Buffer) |
| §3 | Farbdefinitionen RGB565 |
| §4 | Datenstrukturen: `BoardConfig`, `FlightEntry`, Timing-Konstanten |
| §5 | IATA → Englischer Städtenamen-Lookup (Hardcoded-Tabelle, ~60 Einträge) |
| §6 | Hilfsfunktionen: Zeitparsing, NVM-Config laden/speichern, HTML-Escaping |
| §7 | Demo-Daten (7 Abflüge + 7 Ankünfte für Offline-/Setup-Betrieb) |
| §8 | AirLabs API-Abruf, JSON-Streaming-Parser, Codeshare-Gruppierung, Airborne-Erkennung |
| §9 | Pixel-Art-Icons, LED-Matrix-Rendering (`drawBoard`), Scroll-Animation |
| §10 | Web-Interface (HTML inline, WLAN-Scanner-Dropdown, `/save`, `/switch`) |
| §11 | WLAN-Verbindung / Fallback Access-Point, `setup()`, `loop()` |

---

## Wichtige Datenstrukturen

### `BoardConfig`
```cpp
struct BoardConfig {
  String wifiSsid;
  String wifiPass;
  String apiKey;          // AirLabs API-Key
  String iataCode;        // z. B. "HAM"
  int    matrixHeight;    // 64 oder 128 (fest aus Hardware, kein NVM)
  int    switchSeconds;   // Intervall Abflug ↔ Ankunft (Standard: 15 s)
  int    pollMinutes;     // API-Abrufintervall (Standard: 10 min)
  int    brightness;      // 10–255 (Standard: 90)
  bool   codeshareMarquee;// false = nur Operating Carrier, true = Laufschrift aller Nummern
  String timezone;        // POSIX-Zeitzone (Standard: "CET-1CEST,M3.5.0,M10.5.0/3")
};
```

### `FlightEntry`
```cpp
struct FlightEntry {
  String   schedTime;        // "14:50"
  String   operatingFlight;  // "LH2071"
  String   allFlights;       // "LH2071  OS7241  LX3611" (Leerzeichen-getrennt)
  String   cityName;         // "PALMA DE MALLORCA"
  String   gateOrBelt;       // "C7" (Gate) oder "14" (Gepäckband)
  bool     isBaggageBelt;
  String   statusLabel;      // ON TIME | BOARDING | EST 15:16 | DEPARTED | LANDED | APPROACH | CANCELLED | DELAYED
  uint16_t statusColor;      // RGB565
  time_t   sortTs;           // Planzeit als Unix-Timestamp (Sortierschlüssel)
  time_t   eventTs;          // Zeitpunkt Start/Landung/Annullierung (Basis für Anzeigedauer)
  uint8_t  kind;             // 0=normal, 1=gestartet/gelandet, 2=annulliert
};
```

---

## API-Integration (AirLabs)

### Endpoints
| Endpoint | Verwendung |
|---|---|
| `GET /api/v9/schedules?dep_iata=HAM` | Abflüge vom eingestellten Flughafen |
| `GET /api/v9/schedules?arr_iata=HAM` | Ankünfte am eingestellten Flughafen |
| `GET /api/v9/flights?dep_iata=HAM` | Echtzeit-Flugzeuge in der Luft (für Airborne-Erkennung) |

- Verbindung: **HTTPS ohne Zertifikats-Pinning** (`client.setInsecure()`)
- Maximale Flugeinträge im RAM: **12 pro Richtung** (`MAX_FLIGHTS`)
- ArduinoJson-Filter reduziert JSON auf benötigte Felder (~3× kleinere Antwort)
- Streaming-Parser via `WaitingStream`-Klasse (verhindert vorzeitiges EOF bei TLS-Pausen)
- Bis zu **3 Wiederholungsversuche** pro API-Abruf (TLS schlägt direkt nach WLAN-Start gelegentlich fehl)

### Status-Labels und ihre Farben
| Label | Farbe | Bedingung |
|---|---|---|
| `ON TIME` | Grün | Kein Delay oder Delay < 5 min |
| `EST HH:MM` | Amber | Delay > 5 min (Abflug) / > 10 min (Ankunft) |
| `BOARDING` | Grün | (Demo; aus API nicht direkt ableitbar) |
| `APPROACH` | Cyan | Status `active` bei Ankunft, kein signifikanter Delay |
| `DEPARTED` | Cyan | Status `active` bei Abflug, oder via `/flights` als airborne erkannt |
| `LANDED` | Grün | Status `landed` |
| `DELAYED` | Amber | Abflug nicht gestartet, 15 min nach Plan (`DELAYED_AFTER_SEC = 900`) |
| `CANCELLED` | Rot | Status `cancelled` |

### Anzeigedauern nach Ereignis
| Ereignis | Konstante | Dauer |
|---|---|---|
| Abgeflogen (`DEPARTED`) | `SHOW_DEPARTED_SEC` | 5 min |
| Gelandet (`LANDED`) | `SHOW_LANDED_SEC` | 10 min |
| Annulliert (`CANCELLED`) | `SHOW_CANCELLED_SEC` | 60 min |
| Veraltete Abflüge (scheduled, kein Update) | `STALE_DEPARTURE_SEC` | 2 h → ausgeblendet |
| Ankünfte ohne `active`-Status nach Plan | — | 45 min → ausgeblendet |

---

## Display-Rendering

### Pixel-Layout bei 256×64 (nicht aktiv, `matrixHeight < 128`)
```
Y=0..18   : Kopfzeile (Icon, DEPARTURES/ARRIVALS, Uhrzeit, IATA-Badge, Status-Dot)
Y=19      : Trennlinie (Amber)
Y=20..63  : 3 Flugzeilen à 15 px Höhe
```

### Pixel-Layout bei 256×128 (aktuell)
```
Y=0..18   : Kopfzeile
Y=19..29  : Spaltenköpfe (TIME | FLIGHT | DEST/FROM | GATE/BELT | STATUS)
Y=30      : Trennlinie
Y=31..127 : 7 Flugzeilen à 14 px Höhe
```

### Spalten-X-Positionen (beide Auflösungen)
| Spalte | X-Pixel |
|---|---|
| TIME | 2 |
| FLIGHT | 36 (38 px breit) |
| DEST / FROM | 78 (82 px breit) |
| GATE / BELT | 164 |
| STATUS | rechtsbündig bis 255 |

### Scroll-Animation
- Lange Städtenamen (z. B. `PALMA DE MALLORCA`): **Pause-and-Reveal** (kurz statisch, dann sanft nach links scrollen)
- Codeshare-Rotation (`codeshareMarquee = true`): **Pause-and-Slide** — operierender Carrier wird ~4,5 s statisch angezeigt, dann gleitet der Text pixel­genau nach links raus und die nächste Codeshare-Nummer kommt von rechts rein; danach wird der operating Carrier wieder eingeblendet usw. Timing-Konstanten: `CS_PAUSE_FRAMES 50` und `CS_SCROLL_FRAMES 22` (je × 90 ms)
- Animation läuft alle 90 ms (`animFrame++`), nur die scrollenden Zellen werden neu gezeichnet (kein komplettes Neuzeichnen → kein Flackern ohne Double-Buffer)

### Farbpalette (RGB565)
| Konstante | RGB | Bedeutung |
|---|---|---|
| `COL_BG` | (0,0,0) | Hintergrund (schwarz) |
| `COL_HEADER_BG` | (22,16,4) | Kopfzeilen-Hintergrund (sehr dunkles Amber) |
| `COL_AMBER` | (255,190,11) | Zeit, Labels, Städte |
| `COL_WHITE` | (255,255,255) | Flugnummern |
| `COL_GREEN` | (40,255,30) | ON TIME, LANDED |
| `COL_RED` | (255,45,45) | CANCELLED |
| `COL_CYAN` | (0,225,255) | DEPARTED, APPROACH |
| `COL_BELT` | (125,210,255) | Gates (Abflug), Gepäckbänder |
| `COL_DIM` | (150,150,160) | Spaltenköpfe, Status-Dot Demo-Modus |
| `COL_LINE` | (28,24,16) | Trennlinien |

---

## Web-Interface

Erreichbar unter:
- **Normal-Betrieb:** `http://<IP>` oder `http://airportboard.local` (mDNS)
- **Setup-Modus (kein WLAN konfiguriert):** `http://192.168.4.1` (Access-Point `AIRPORT-BOARD-SETUP`)

### HTTP-Routen
| Route | Methode | Funktion |
|---|---|---|
| `/` | GET | Einstellungsseite mit WLAN-Scanner-Dropdown |
| `/save` | POST | Einstellungen speichern (WLAN → Neustart, Rest → sofort) |
| `/switch` | POST | Manuell zwischen DEPARTURES und ARRIVALS umschalten |

### Konfigurierbare Parameter (via Web)
| Parameter | Typ | Bereich | Standard |
|---|---|---|---|
| WLAN-SSID | String | Scan-Dropdown | — |
| WLAN-Passwort | String | — | — |
| AirLabs API-Key | String | ≥10 Zeichen | aus `secrets.h` |
| Flughafen IATA | 3-Zeichen | A–Z | `HAM` |
| Wechselintervall | Int | 4–120 s | 15 s |
| API-Update-Intervall | Int | 2–60 min | 10 min |
| LED-Helligkeit | Int | 10–255 | 90 |
| Codeshare-Anzeige | Bool | 0/1 | 0 (nur Operating) |

Alle Einstellungen werden in NVM (`Preferences`, Namespace `airport_cfg`) persistent gespeichert.

---

## Startsequenz (`setup()`)

1. WLAN-Treiber initialisieren (**vor** Display-Init, sonst scheitert `esp_wifi_init` wegen DMA-RAM-Verbrauch)
2. HUB75E DMA-Panel initialisieren
3. `VirtualMatrixPanel_T` (GFX-Wrapper) erstellen
4. Demo-Daten laden + erstes Rendering
5. WLAN verbinden oder Access-Point starten
6. Webserver starten
7. Bei WLAN: NTP-Zeit synchronisieren, AirLabs API abrufen, 20 s Infoschirm zeigen

---

## Hauptschleife (`loop()`)

| Aufgabe | Intervall |
|---|---|
| Web-Requests bedienen | Jede Iteration |
| Infoschirm ausblenden | Einmalig nach 20 s |
| DEPARTURES ↔ ARRIVALS wechseln | `cfg.switchSeconds` |
| AirLabs API neu abrufen | `cfg.pollMinutes` |
| Scroll-Animation + Uhrzeit-Update | 90 ms |

---

## Datei-Struktur

```
Departure-Arrival-Matrix-Board/
├── src/
│   └── main.cpp              ← gesamte Firmware (~1460 Zeilen)
├── include/
│   ├── secrets.example.h     ← Vorlage für API-Key
│   └── secrets.h             ← lokal, nicht eingecheckt (.gitignore)
├── platformio.ini            ← Board, Framework, Libraries, Build-Flags
├── README.md                 ← Kurzanleitung
└── AGENTS.md                 ← diese Datei
```

---

## Bekannte Einschränkungen & Fallstricke

- **Heap-Fragmentierung:** TLS/HTTPS braucht ~40 KB zusammenhängenden RAM.
  API-Abrufe dürfen **nicht** während eines aktiven HTTP-Request-Handlers im Webserver laufen
  (deshalb `refreshRequested`-Flag: Abruf erst in `loop()`).
- **Double-Buffer deaktiviert** (`BOARD_DOUBLE_BUFFER 0`) auf Standard-ESP32, da zu wenig PSRAM.
  Für ESP32-S3 kann `1` gesetzt werden.
- **Kein PSRAM genutzt:** Der Code verwendet ausschließlich internen DRAM.
- **ArduinoJson-Kompatibilität:** Code unterstützt v6 (`DynamicJsonDocument`) und v7 (`JsonDocument`) via `#if`.
- **IATA-Städtenamen:** Fest codierte Tabelle (~60 Einträge). Unbekannte Codes werden als IATA-Code angezeigt.
- **Zeitzone:** Einstellbar als POSIX-String; Standard ist `CET-1CEST,M3.5.0,M10.5.0/3` (Deutschland).

---

## Typische Änderungsszenarien

### Neuen Flughafen als Vorgabe setzen
Konstante in `loadConfig()` ändern:
```cpp
cfg.iataCode = prefs.getString("iata", "FRA"); // statt "HAM"
```

### Neuen Städtenamen zur Lookup-Tabelle hinzufügen
In `CITY_LOOKUP[]` in §5 eintragen:
```cpp
{"XXX", "STADTNAME"},
```

### Panel-Anordnung (aktuell 256×128, U-Form)
1. `BOARD_PANEL_ROWS` = `2`, `BOARD_PANEL_COLS` = `2`, `BOARD_PANEL_CHAIN` = `4`
2. `BOARD_CHAIN_TYPE` = `CHAIN_TOP_LEFT_DOWN` (untere Reihe kopfüber). Alle Panels aufrecht wäre `CHAIN_TOP_LEFT_DOWN_ZZ`
3. `cfg.matrixHeight` kommt aus `BOARD_SCREEN_H` → 128 → 7-Zeilen-Layout

### Timing-Konstanten anpassen
Alle Zeitregeln stehen direkt unterhalb der `FlightEntry`-Struct:
```cpp
#define SHOW_DEPARTED_SEC 300
#define SHOW_LANDED_SEC   600
#define SHOW_CANCELLED_SEC 3600
#define MAX_FINISHED_ROWS 2
#define DELAYED_AFTER_SEC 900
#define STALE_DEPARTURE_SEC 7200
```
