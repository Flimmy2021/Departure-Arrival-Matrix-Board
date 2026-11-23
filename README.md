# Departure-Arrival-Matrix-Board

Flughafen-Abflug- und Ankunftstafel (Departures / Arrivals) auf einer HUB75/HUB75E-LED-Matrix mit ESP32.
Die Flugdaten kommen live von der [AirLabs](https://airlabs.co) Schedules-API; ohne API-Key oder WLAN läuft ein Demo-Modus.

## Hardware

- ESP32 (oder ESP32-S3)
- 8x HUB75E-Panels 64x64 (2 Reihen à 4 Panels = 256x128) oder 4x 64x64 (256x64)
- Pinbelegung: siehe `BOARD_*_PIN` in [src/main.cpp](src/main.cpp)
- Verkettung der zwei Reihen über `BOARD_CHAIN_TYPE` einstellbar (Serpentine oder Zickzack)

## Einrichtung

1. Projekt mit PlatformIO öffnen und flashen (`pio run -t upload`).
2. AirLabs API-Key entweder
   - `include/secrets.example.h` nach `include/secrets.h` kopieren und den Key eintragen (wird nicht eingecheckt), oder
   - später im Web-Interface eintragen.
3. Beim ersten Start öffnet das Board das WLAN `AIRPORT-BOARD-SETUP`. Verbinden und `http://192.168.4.1` aufrufen.
4. Im Web-Interface WLAN, Flughafen (IATA), Matrix-Auflösung, Wechselzeit, Helligkeit usw. einstellen.
   Danach ist das Board unter `http://airportboard.local` erreichbar.

## Projektstruktur

| Pfad | Inhalt |
| --- | --- |
| `src/main.cpp` | Firmware (API, Rendering, Web-Interface) |
| `include/` | Header, z. B. `secrets.h` (lokal) |
| `platformio.ini` | Board, Framework und Bibliotheken |