# Departure-Arrival-Matrix-Board

Flughafen-Abflug- und Ankunftstafel (Departures / Arrivals) auf einer HUB75/HUB75E-LED-Matrix mit ESP32.
Die Flugdaten kommen live von der [AirLabs](https://airlabs.co) Schedules-API; ohne API-Key oder WLAN läuft ein Demo-Modus.

## Hardware

- ESP32 (esp32dev)
- 4x HUB75E-Panels 128x64 als U (2 Spalten × 2 Reihen, untere Reihe kopfüber) = 256x128 (7 Flugzeilen)
- Pinbelegung: siehe `BOARD_*_PIN` in [src/main.cpp](src/main.cpp)

## Einrichtung

1. Projekt mit PlatformIO öffnen und flashen (`pio run -t upload`).
2. AirLabs API-Key entweder
   - `include/secrets.example.h` nach `include/secrets.h` kopieren und den Key eintragen (wird nicht eingecheckt), oder
   - später im Web-Interface eintragen.
3. Beim ersten Start öffnet das Board das WLAN `AIRPORT-BOARD-SETUP`. Verbinden und `http://192.168.4.1` aufrufen.
4. Im Web-Interface WLAN, Flughafen (IATA), Wechselzeit, Helligkeit usw. einstellen.
   Danach ist das Board unter `http://airportboard.local` erreichbar.

## Projektstruktur

| Pfad | Inhalt |
| --- | --- |
| `src/main.cpp` | Firmware (API, Rendering, Web-Interface) |
| `include/` | Header, z. B. `secrets.h` (lokal) |
| `platformio.ini` | Board, Framework und Bibliotheken |