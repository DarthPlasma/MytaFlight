# MyTAflight — ADS-B injector (ESP32)

Bench-test the **Feature C ADS-B** OSD elements and threat logic without real air traffic. An
ESP32-WROOM pretends to be an **Aerobit TT-SC1** (or any MAVLink ADS-B receiver): it raises its
own WiFi hotspot, serves a web page where you define up to **5 aircraft**, and streams MAVLink v1
`ADSB_VEHICLE` (msg 246) frames out a UART at ~2 Hz — exactly what `telemetry/mavlink.c` decodes.

Because ADS-B threat prioritization (nearest time-to-arrival within the cone) only matters with
several aircraft, the injector sends up to 5 at once (Betaflight's `ADSB_MAX_VEHICLES`) and lets
you place them relative to a home point, optionally pointed straight at you ("approaching").

**Moving targets:** press **Avvia** and each aircraft flies along its heading at its ground speed
(and climbs/descends at its V/S) — the position advances every tick and keeps being transmitted.
A live table shows each aircraft's distance / bearing / altitude / time-to-arrival updating in real
time, plus a **Minaccia** flag computed the same way Betaflight does (within the cone AND ToA ≤
threshold), so you can watch a threat develop and cross-check it against what the OSD shows.
**Pausa** freezes them; **Reset posizioni** returns every aircraft to the values from the last
Aggiorna. The cone (°) and ToA-max (s) inputs should mirror your FC's `adsb_detection_cone` /
`adsb_aircraft_toa` so the predictor matches the firmware.

## Wiring
```
ESP32 GPIO17 (TX2) ─────────────▶ FC UART RX pad
ESP32 GND        ───────────────  FC GND
```
TX-only — the injector never reads from the FC. Use any spare FC UART.

## Flight controller setup
1. **Firmware**: built with `USE_ADSB` (the build tool ticks "ADS-B traffic"). ADS-B needs GPS.
2. **Ports tab**: on the UART you wired, set **Telemetry = MAVLink**, baud **115200** (must match
   `UART_BAUD` in the sketch).
3. **GPS fix required**: Betaflight computes each aircraft's distance/bearing from its *own* GPS
   position (`isEnvironmentOkForCalculatingADSBDistanceBearing` needs a fix with >4 sats). On the
   bench either get a real fix, or test in **SITL** (virtual GPS) — the injector works there too,
   wired to a SITL serial port instead of a physical UART.
4. Add the ADS-B OSD elements (`osd_adsb_warning_pos`, `osd_adsb_info_pos`, `osd_adsb_status_pos`),
   and optionally tune `adsb_max_dist_horiz` / `adsb_max_dist_vert` / `adsb_detection_cone` /
   `adsb_aircraft_toa`.

## Using it
1. Flash `adsb-injector.ino` (Arduino IDE, board "ESP32 Dev Module"; no external libraries).
2. Join WiFi **`MyTAflight-ADSB`** (password `adsbtest`), open **http://192.168.4.1**.
3. Set your **home** lat/lon (roughly where the FC thinks it is).
4. Fill each aircraft, or use the **helper**: pick a slot, a distance + bearing from home, tick
   *in avvicinamento* to auto-aim its heading back at you, press **Calcola lat/lon**, then
   **Aggiorna**. The ESP32 keeps transmitting the enabled aircraft so they don't time out.

### Fields
`Callsign · Lat · Lon · Alt AMSL (m) · Velocità (km/h) · Heading (°) · V/S (m/s) · Tipo`.
Units are converted to MAVLink's on the wire (deg→degE7, m→mm, km/h→cm/s, °→cdeg). ICAO is
auto-assigned per slot (`0xA00001+n`) so each slot is a distinct tracked vehicle. `Tipo` is the
MAVLink `ADSB_EMITTER_TYPE` (Light/Small/Large/Heavy/Rotorcraft/…).

### Testing the threat logic (with movement)
Enable two aircraft: one **closer but receding** (heading away) and one **farther but approaching
fast within the cone** (helper with *in avvicinamento*), press **Avvia**, and watch. As the second
closes in, its live ToA falls until it flags **Minaccia**; confirm the OSD arrow/info and the
`AIRCRAFT APPROACHING <s>` warning both switch to track the *approaching* one, not the closest —
the behaviour added in the threat-priority fix — and that the OSD's ToA tracks the injector's.

## Validation
The logic that can't be tested without ESP32 hardware is verified on the host:
- **`adsb_wire_test.c`** — the exact bytes the sketch emits, decoded by Betaflight's OWN bundled
  MAVLink parser (byte layout + X25 CRC + CRC_EXTRA 184). `PASS: 10/10`.
- **`motion_test.c`** — the movement/geometry/threat-predictor logic (copied verbatim from the
  sketch): an approaching aircraft's distance and ToA fall and it flags as a threat; a receding one
  never does.

Re-run both from the repo root:
```
clang -w -I lib/main/MAVLink mytaflight-tools/adsb-injector/adsb_wire_test.c -o /tmp/awt -lm && /tmp/awt
clang -w mytaflight-tools/adsb-injector/motion_test.c -o /tmp/mt -lm && /tmp/mt
```
(Both replicate the sketch's code verbatim — keep them in sync if you change the encoder or motion.)
