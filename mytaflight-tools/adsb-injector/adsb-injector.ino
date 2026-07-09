/*
 * MyTAflight — ADS-B injector for ESP32-WROOM.
 *
 * Simulates an Aerobit TT-SC1 (or any MAVLink ADS-B receiver) so you can bench-test the
 * Feature C ADS-B OSD/threat code without real traffic. The ESP32 raises a WiFi access point
 * and serves a web page where you define up to 5 aircraft (position/speed/heading/type/callsign,
 * or placed relative to a home point), then streams MAVLink v1 ADSB_VEHICLE (msg 246) frames out
 * a UART at ~2 Hz — exactly what Betaflight's telemetry/mavlink.c decodes.
 *
 * MOVING TARGETS: press Avvia and each aircraft flies along its heading at its ground speed
 * (and climbs/descends at its V/S). A live table shows distance / bearing / time-to-arrival
 * updating in real time, so you can watch a threat approach and cross-check it against the OSD.
 *
 * Wire format is verified byte-for-byte against Betaflight's own bundled MAVLink library
 * (lib/main/MAVLink/common/mavlink_msg_adsb_vehicle.h): LEN 38, CRC_EXTRA 184, fields ordered
 * by size. Betaflight's mavlink_parse_char accepts v1, and ADSB_VEHICLE needs no heartbeat.
 * See adsb_wire_test.c for the host-side proof.
 *
 * Wiring:  ESP32 TX pin (default GPIO17) --> FC UART RX pad.   GND <--> GND.  (TX-only: we only send.)
 * FC setup: Ports tab -> set that UART's Telemetry to "MAVLink", baud = UART_BAUD below.
 *           A GPS fix is required for BF to compute distance/bearing (or use SITL virtual GPS).
 *
 * Build: Arduino IDE, board "ESP32 Dev Module" (or your WROOM board). No external libraries.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <string.h>
#include <math.h>

// ---------------- configuration ----------------
static const char *AP_SSID = "MyTAflight-ADSB";
static const char *AP_PASS = "adsbtest";     // >= 8 chars for WPA2
static const uint32_t UART_BAUD = 115200;    // MUST match the FC's MAVLink serial port baud
static const int UART_TX_PIN = 17;           // ESP32 -> FC RX
static const int UART_RX_PIN = 16;           // unused (we only transmit)
static const uint32_t TX_PERIOD_MS = 500;    // resend every 0.5s (BF frees a vehicle after 10s of silence)
#define NUM_AC 5                              // Betaflight ADSB_MAX_VEHICLES

static const double M_PER_DEG_LAT = 111320.0;

HardwareSerial FCserial(2);                   // UART2
WebServer server(80);

// ---------------- aircraft state ----------------
struct Aircraft {
  bool     enabled;
  double   lat, lon;      // current position, degrees (advances while sim runs)
  double   altAmslM;      // current altitude AMSL, meters
  double   initLat, initLon, initAlt;  // captured on Aggiorna, restored by Reset
  int32_t  speedKmh;      // ground speed
  int32_t  headingDeg;    // course over ground (also the direction of travel)
  int32_t  vsMps;         // vertical speed, m/s (positive = climbing)
  uint8_t  emitter;       // ADSB_EMITTER_TYPE (0..19)
  char     callsign[9];
};

Aircraft ac[NUM_AC];
uint32_t framesSent = 0;
double homeLat = 45.0000000, homeLon = 9.0000000;
int    coneDeg = 20;      // for the live "threat?" predictor (BF default detection cone width)
int    toaMaxS = 60;      // for the live ToA threshold (BF ap default)
bool   simRunning = false;
uint32_t lastMotionMs = 0;

// ADSB_FLAGS bits (from MAVLink common.h)
static const uint16_t ADSB_VALID_COORDS   = 1;
static const uint16_t ADSB_VALID_ALTITUDE = 2;
static const uint16_t ADSB_VALID_HEADING  = 4;
static const uint16_t ADSB_VALID_VELOCITY = 8;
static const uint16_t ADSB_VALID_CALLSIGN = 16;

// ---------------- MAVLink v1 ADSB_VEHICLE encoder (verified against BF's lib — DO NOT edit
//                  without re-running adsb_wire_test.c) ----------------
static uint8_t txSeq = 0;

static void crcAccumulate(uint8_t data, uint16_t *crc) {
  uint8_t tmp = data ^ (uint8_t)(*crc & 0xff);
  tmp ^= (tmp << 4);
  *crc = (*crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4);
}

// All multi-byte fields are little-endian (both the ESP32 and MAVLink are LE).
static void sendAdsbVehicle(uint32_t icao, int32_t latE7, int32_t lonE7, int32_t altMm,
                            uint16_t hdgCd, uint16_t horVelCms, int16_t verVelCms,
                            uint16_t flags, uint8_t emitter, const char *callsign) {
  uint8_t p[38];
  memset(p, 0, sizeof(p));
  memcpy(p + 0,  &icao,      4);
  memcpy(p + 4,  &latE7,     4);
  memcpy(p + 8,  &lonE7,     4);
  memcpy(p + 12, &altMm,     4);
  memcpy(p + 16, &hdgCd,     2);
  memcpy(p + 18, &horVelCms, 2);
  memcpy(p + 20, &verVelCms, 2);
  memcpy(p + 22, &flags,     2);
  for (int i = 0; i < 9; i++) p[27 + i] = (i < 8 && callsign[i]) ? (uint8_t)callsign[i] : 0;
  p[36] = emitter;
  p[37] = 0;

  const uint8_t hdr[6] = {0xFE, 38, txSeq++, 0x01, 0x01, 246};
  uint16_t crc = 0xFFFF;
  for (int i = 1; i < 6; i++) crcAccumulate(hdr[i], &crc);
  for (int i = 0; i < 38; i++) crcAccumulate(p[i], &crc);
  crcAccumulate(184, &crc);
  const uint8_t cka = crc & 0xFF, ckb = crc >> 8;

  FCserial.write(hdr, 6);
  FCserial.write(p, 38);
  FCserial.write(cka);
  FCserial.write(ckb);
  framesSent++;
}

static void transmitAll() {
  for (int i = 0; i < NUM_AC; i++) {
    if (!ac[i].enabled) continue;
    const uint32_t icao = 0xA00001 + i;
    const int32_t  latE7 = (int32_t)llround(ac[i].lat * 1e7);
    const int32_t  lonE7 = (int32_t)llround(ac[i].lon * 1e7);
    const int32_t  altMm = (int32_t)llround(ac[i].altAmslM * 1000.0);
    const uint16_t hdgCd = (uint16_t)((((ac[i].headingDeg % 360) + 360) % 360) * 100);
    const uint16_t horVelCms = (uint16_t)llround(ac[i].speedKmh * (100000.0 / 3600.0));
    const int16_t  verVelCms = (int16_t)(ac[i].vsMps * 100);
    const uint16_t flags = ADSB_VALID_COORDS | ADSB_VALID_ALTITUDE | ADSB_VALID_HEADING |
                           ADSB_VALID_VELOCITY | ADSB_VALID_CALLSIGN;
    sendAdsbVehicle(icao, latE7, lonE7, altMm, hdgCd, horVelCms, verVelCms, flags,
                    ac[i].emitter, ac[i].callsign);
  }
}

// ---------------- motion + geometry ----------------
static void integrateMotion(double dtSec) {
  for (int i = 0; i < NUM_AC; i++) {
    if (!ac[i].enabled) continue;
    const double mps = ac[i].speedKmh / 3.6;
    const double dist = mps * dtSec;                 // metres travelled this tick
    const double rad = ac[i].headingDeg * M_PI / 180.0;
    ac[i].lat += (dist * cos(rad)) / M_PER_DEG_LAT;
    ac[i].lon += (dist * sin(rad)) / (M_PER_DEG_LAT * cos(ac[i].lat * M_PI / 180.0));
    ac[i].altAmslM += ac[i].vsMps * dtSec;
  }
}

// Flat-earth distance (m) and bearing (deg, 0=N CW) from home to an aircraft.
static void distBearing(int i, double *distM, double *brgDeg) {
  const double dN = (ac[i].lat - homeLat) * M_PER_DEG_LAT;
  const double dE = (ac[i].lon - homeLon) * M_PER_DEG_LAT * cos(homeLat * M_PI / 180.0);
  *distM = sqrt(dN * dN + dE * dE);
  double b = atan2(dE, dN) * 180.0 / M_PI;
  if (b < 0) b += 360.0;
  *brgDeg = b;
}

// Mirror BF's threat test (cone + ToA) so the live table predicts what the OSD should show.
static bool isThreatCandidate(int i, double distM, double brgDeg, double *toaOut) {
  const double mps = ac[i].speedKmh / 3.6;
  const double toa = (mps > 0.1) ? (distM / mps) : 1e9;
  *toaOut = toa;
  if (toa > toaMaxS) return false;
  double hErr = ac[i].headingDeg - (brgDeg + 180.0);   // heading vs reciprocal bearing
  hErr = fmod(fmod(hErr, 360.0) + 360.0, 360.0);
  if (hErr > 180.0) hErr -= 360.0;
  return fabs(hErr) <= coneDeg / 2.0;
}

// ---------------- web UI ----------------
static const char *EMITTERS[] = {
  "0 Sconosciuto", "1 Leggero", "2 Piccolo", "3 Grande", "4 Grande (scia)", "5 Pesante",
  "6 Alta manovra", "7 Elicottero", "8 -", "9 Aliante", "10 Dirigibile", "11 Paracadute",
  "12 Ultraleggero", "13 -", "14 Drone (UAV)", "15 Spaziale", "16 -", "17 Veic. emergenza",
  "18 Veic. servizio", "19 Ostacolo"
};
static const int EMITTER_COUNT = sizeof(EMITTERS) / sizeof(EMITTERS[0]);

static String buildPage() {
  String h;
  h.reserve(11000);
  h += F("<!doctype html><html lang=it><head><meta charset=utf-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>MyTAflight ADS-B injector</title><style>"
         "body{font:14px system-ui,sans-serif;background:#12151c;color:#e6e9ef;margin:0;padding:14px}"
         "h1{font-size:16px}h2{font-size:12px;color:#8b93a3;text-transform:uppercase;letter-spacing:.05em;margin:16px 0 6px}"
         ".panel{background:#1b2029;border:1px solid #2c333f;border-radius:8px;padding:12px;margin-bottom:14px}"
         "input,select{background:#0c0f15;color:#e6e9ef;border:1px solid #2c333f;border-radius:5px;padding:5px 6px;font:inherit}"
         "input[type=number]{width:96px}input.cs{width:90px}label{color:#8b93a3;font-size:12px}"
         ".ac{border:1px solid #2c333f;border-radius:6px;padding:8px;margin-bottom:8px}"
         ".ac .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:6px 12px;margin-top:6px}"
         ".fld{display:flex;flex-direction:column;gap:2px}"
         "button{background:#4ea1ff;color:#04101f;border:0;border-radius:6px;padding:9px 18px;font-weight:700;cursor:pointer;font:inherit}"
         "button.ghost{background:#2c333f;color:#e6e9ef}.muted{color:#8b93a3;font-size:12px}"
         "table{border-collapse:collapse;width:100%;font-size:13px}th,td{border:1px solid #2c333f;padding:4px 8px;text-align:right}"
         "th{color:#8b93a3;font-weight:600}td.l,th.l{text-align:left}tr.threat td{background:rgba(255,93,93,.18)}"
         "#run{background:#3ad29f}#run.on{background:#ffb454}"
         "</style></head><body><h1>MyTAflight — ADS-B injector</h1>");
  h += F("<p class=muted>UART2 TX=GPIO"); h += UART_TX_PIN; h += F(" @"); h += UART_BAUD;
  h += F(" baud &middot; MAVLink v1 ADSB_VEHICLE &middot; frame: <span id=frames>?</span></p>");

  // Live simulation panel
  h += F("<div class=panel><h2>Simulazione</h2>"
         "<div style='display:flex;gap:10px;align-items:center;flex-wrap:wrap'>"
         "<button id=run onclick='sim(\"toggle\")'>Avvia</button>"
         "<button class=ghost onclick='sim(\"reset\")'>Reset posizioni</button>"
         "<span class=muted>Cono &deg;</span><input type=number id=cone style=width:70px value=");
  h += coneDeg; h += F("><span class=muted>ToA max (s)</span><input type=number id=toa style=width:70px value=");
  h += toaMaxS; h += F("></div>"
         "<table style='margin-top:10px'><thead><tr><th class=l>#</th><th class=l>Callsign</th>"
         "<th>Dist (m)</th><th>Rilev (&deg;)</th><th>Quota (m)</th><th>ToA (s)</th><th>Minaccia</th></tr></thead>"
         "<tbody id=live></tbody></table>"
         "<p class=muted>Le posizioni avanzano lungo heading a velocit&agrave; impostata. \"Minaccia\" = cono+ToA (come li calcola BF).</p></div>");

  h += F("<form method=POST action=/update>");
  // Home + relative-placement helper
  h += F("<div class=panel><h2>Posizione home (riferimento)</h2><div style='display:flex;gap:12px;flex-wrap:wrap'>"
         "<div class=fld><label>Lat home</label><input type=number step=any name=homelat value='");
  h += String(homeLat, 7);
  h += F("'></div><div class=fld><label>Lon home</label><input type=number step=any name=homelon value='");
  h += String(homeLon, 7);
  h += F("'></div></div>"
         "<h2 style='margin-top:12px'>Piazza un velivolo (helper)</h2>"
         "<div style='display:flex;gap:10px;align-items:end;flex-wrap:wrap'>"
         "<div class=fld><label>Velivolo</label><select id=hrow>");
  for (int i = 0; i < NUM_AC; i++) { h += F("<option value="); h += i; h += F(">#"); h += (i + 1); h += F("</option>"); }
  h += F("</select></div>"
         "<div class=fld><label>Distanza (m)</label><input type=number id=hdist value=3000></div>"
         "<div class=fld><label>Rilevamento (&deg;)</label><input type=number id=hbrg value=45></div>"
         "<div class=fld><label><input type=checkbox id=happ checked> in avvicinamento</label></div>"
         "<button type=button class=ghost onclick=place()>Calcola lat/lon</button></div>"
         "<p class=muted>Riempie lat/lon (e heading se \"in avvicinamento\") del velivolo scelto. Poi Aggiorna.</p></div>");

  // Aircraft cards
  h += F("<div class=panel><h2>Velivoli (parametri di partenza)</h2>");
  for (int i = 0; i < NUM_AC; i++) {
    h += F("<div class=ac><label><input type=checkbox name=en"); h += i;
    if (ac[i].enabled) h += F(" checked");
    h += F("> Velivolo #"); h += (i + 1); h += F(" &middot; ICAO 0x"); h += String(0xA00001 + i, HEX);
    h += F("</label><div class=grid>");
    h += F("<div class=fld><label>Callsign</label><input class=cs maxlength=8 name=cs"); h += i;
    h += F(" value='"); h += ac[i].callsign; h += F("'></div>");
    h += F("<div class=fld><label>Lat</label><input type=number step=any id=lat"); h += i;
    h += F(" name=lat"); h += i; h += F(" value='"); h += String(ac[i].lat, 7); h += F("'></div>");
    h += F("<div class=fld><label>Lon</label><input type=number step=any id=lon"); h += i;
    h += F(" name=lon"); h += i; h += F(" value='"); h += String(ac[i].lon, 7); h += F("'></div>");
    h += F("<div class=fld><label>Alt AMSL (m)</label><input type=number name=alt"); h += i;
    h += F(" value="); h += (int32_t)llround(ac[i].altAmslM); h += F("></div>");
    h += F("<div class=fld><label>Velocit&agrave; (km/h)</label><input type=number name=spd"); h += i;
    h += F(" value="); h += ac[i].speedKmh; h += F("></div>");
    h += F("<div class=fld><label>Heading (&deg;)</label><input type=number id=hdg"); h += i;
    h += F(" name=hdg"); h += i; h += F(" value="); h += ac[i].headingDeg; h += F("></div>");
    h += F("<div class=fld><label>V/S (m/s)</label><input type=number name=vs"); h += i;
    h += F(" value="); h += ac[i].vsMps; h += F("></div>");
    h += F("<div class=fld><label>Tipo</label><select name=type"); h += i; h += F(">");
    for (int e = 0; e < EMITTER_COUNT; e++) {
      h += F("<option value="); h += e;
      if (ac[i].emitter == e) h += F(" selected");
      h += F(">"); h += EMITTERS[e]; h += F("</option>");
    }
    h += F("</select></div></div></div>");
  }
  h += F("</div><button type=submit>Aggiorna (imposta e azzera posizioni)</button></form>");

  // scripts: placement helper + live poll + sim controls
  h += F("<script>"
         "function place(){var r=+hrow.value,d=+hdist.value,b=+hbrg.value,"
         "hla=+document.querySelector('[name=homelat]').value,hlo=+document.querySelector('[name=homelon]').value,"
         "rad=b*Math.PI/180,dLat=(d*Math.cos(rad))/111320,dLon=(d*Math.sin(rad))/(111320*Math.cos(hla*Math.PI/180));"
         "document.getElementById('lat'+r).value=(hla+dLat).toFixed(7);"
         "document.getElementById('lon'+r).value=(hlo+dLon).toFixed(7);"
         "if(happ.checked)document.getElementById('hdg'+r).value=Math.round((b+180)%360);}"
         "function sim(cmd){fetch('/sim?cmd='+cmd+'&cone='+cone.value+'&toa='+toa.value,{method:'POST'}).then(poll);}"
         "function poll(){fetch('/state').then(r=>r.json()).then(s=>{"
         "run.textContent=s.running?'Pausa':'Avvia';run.className=s.running?'on':'';"
         "frames.textContent=s.frames;"
         "var b='';for(var a of s.ac){if(!a.en)continue;"
         "b+='<tr'+(a.threat?' class=threat':'')+'><td class=l>#'+(a.i+1)+'</td><td class=l>'+a.cs+"
         "'</td><td>'+a.dist+'</td><td>'+a.brg+'</td><td>'+a.alt+'</td><td>'+a.toa+'</td><td>'+(a.threat?'\\u26a0':'')+'</td></tr>';}"
         "live.innerHTML=b||'<tr><td class=l colspan=7>nessun velivolo attivo</td></tr>';});}"
         "setInterval(poll,1000);poll();</script></body></html>");
  return h;
}

static int argInt(const String &n, int def) { return server.hasArg(n) ? server.arg(n).toInt() : def; }
static double argDouble(const String &n, double def) { return server.hasArg(n) ? server.arg(n).toDouble() : def; }

static void handleUpdate() {
  homeLat = argDouble("homelat", homeLat);
  homeLon = argDouble("homelon", homeLon);
  for (int i = 0; i < NUM_AC; i++) {
    ac[i].enabled    = server.hasArg("en" + String(i));
    ac[i].lat        = argDouble("lat" + String(i), ac[i].lat);
    ac[i].lon        = argDouble("lon" + String(i), ac[i].lon);
    ac[i].altAmslM   = argInt("alt" + String(i), (int)ac[i].altAmslM);
    ac[i].speedKmh   = argInt("spd" + String(i), ac[i].speedKmh);
    ac[i].headingDeg = argInt("hdg" + String(i), ac[i].headingDeg);
    ac[i].vsMps      = argInt("vs" + String(i), ac[i].vsMps);
    int e = argInt("type" + String(i), ac[i].emitter);
    ac[i].emitter    = (e < 0 || e > 19) ? 0 : (uint8_t)e;
    String cs = server.arg("cs" + String(i));
    memset(ac[i].callsign, 0, sizeof(ac[i].callsign));
    for (int k = 0; k < 8 && k < (int)cs.length(); k++) ac[i].callsign[k] = cs[k];
    // capture as the initial state so Reset returns here
    ac[i].initLat = ac[i].lat; ac[i].initLon = ac[i].lon; ac[i].initAlt = ac[i].altAmslM;
  }
  transmitAll();
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleSim() {
  if (server.hasArg("cone")) coneDeg = server.arg("cone").toInt();
  if (server.hasArg("toa"))  toaMaxS = server.arg("toa").toInt();
  const String cmd = server.arg("cmd");
  if (cmd == "toggle") { simRunning = !simRunning; lastMotionMs = millis(); }
  else if (cmd == "reset") {
    for (int i = 0; i < NUM_AC; i++) { ac[i].lat = ac[i].initLat; ac[i].lon = ac[i].initLon; ac[i].altAmslM = ac[i].initAlt; }
    transmitAll();
  }
  server.send(200, "text/plain", "ok");
}

static void handleState() {
  String j = "{\"running\":"; j += simRunning ? "true" : "false";
  j += ",\"frames\":"; j += framesSent; j += ",\"ac\":[";
  for (int i = 0; i < NUM_AC; i++) {
    double dist, brg, toa;
    distBearing(i, &dist, &brg);
    const bool threat = ac[i].enabled && isThreatCandidate(i, dist, brg, &toa);
    if (i) j += ",";
    j += "{\"i\":"; j += i;
    j += ",\"en\":"; j += ac[i].enabled ? "true" : "false";
    j += ",\"cs\":\""; j += ac[i].callsign; j += "\"";
    j += ",\"dist\":"; j += (int32_t)llround(dist);
    j += ",\"brg\":"; j += (int32_t)llround(brg);
    j += ",\"alt\":"; j += (int32_t)llround(ac[i].altAmslM);
    j += ",\"toa\":"; j += (toa > 99999 ? 99999 : (int32_t)llround(toa));
    j += ",\"threat\":"; j += threat ? "true" : "false";
    j += "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void setup() {
  Serial.begin(115200);
  FCserial.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  for (int i = 0; i < NUM_AC; i++) {
    ac[i] = {false, homeLat, homeLon, 500, homeLat, homeLon, 500, 300, 0, 0, 3, {0}};
  }
  // seed one airliner 3 km NE, approaching at 300 km/h
  ac[0].enabled = true;
  strcpy(ac[0].callsign, "IBE31VM");
  ac[0].lat = ac[0].initLat = homeLat + 3000.0 * cos(45 * M_PI / 180) / M_PER_DEG_LAT;
  ac[0].lon = ac[0].initLon = homeLon + 3000.0 * sin(45 * M_PI / 180) / (M_PER_DEG_LAT * cos(homeLat * M_PI / 180));
  ac[0].headingDeg = 225;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP up: "); Serial.print(AP_SSID);
  Serial.print("  http://"); Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", buildPage()); });
  server.on("/update", HTTP_POST, handleUpdate);
  server.on("/sim", HTTP_POST, handleSim);
  server.on("/state", HTTP_GET, handleState);
  server.begin();
  lastMotionMs = millis();
}

void loop() {
  server.handleClient();
  const uint32_t now = millis();

  if (simRunning) {
    const double dt = (now - lastMotionMs) / 1000.0;
    if (dt > 0) { integrateMotion(dt); lastMotionMs = now; }
  } else {
    lastMotionMs = now;  // keep dt from jumping when resumed
  }

  static uint32_t lastTx = 0;
  if (now - lastTx >= TX_PERIOD_MS) { lastTx = now; transmitAll(); }
}
