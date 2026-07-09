#!/usr/bin/env python3
"""
MyTAflight build tool.

A tiny local web UI to build firmware for the project's targets, modeled on
Betaflight's own official cloud-build page: pick a target, a radio protocol,
a telemetry protocol, an OSD system, a motor protocol, and any extra features,
then Build. It always builds CLOUD_BUILD-style (only what you pick is
compiled in) and drives `make <TARGET> OPTIONS="..."` in the repo.

Run:  python3 mytaflight-tools/build-tool.py    then open http://localhost:8792
"""

import glob
import json
import os
import re
import subprocess
import http.server

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root = parent of mytaflight-tools/
PORT = 8792

# Only these targets, as requested.
TARGETS = ["DAKEFPVH743", "TMOTORVELOXF7V2", "KAKUTEH7", "MAMBAH743", "MAMBAH743_2022B"]

# Whitelist of build flags the UI (and free-text custom defines) may pass. Guards the make
# invocation. Allows an optional "=VALUE" for defines that take a value (e.g. TARGET_FLASH_SIZE=2048).
ALLOWED = re.compile(r"^[A-Z0-9_]+(=[A-Za-z0-9_.]+)?$")


def run_build(target, options):
    if target not in TARGETS:
        return {"ok": False, "cmd": "", "log": "Unknown target."}
    for o in options:
        if not ALLOWED.match(o):
            return {"ok": False, "cmd": "", "log": "Illegal option: " + o}

    cmd = ["make", target]
    if options:
        cmd.append("OPTIONS=" + " ".join(options))
    shown = " ".join(cmd[:2]) + (' OPTIONS="' + " ".join(options) + '"' if options else "")

    try:
        proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return {"ok": False, "cmd": shown, "log": "Build timed out (600s)."}

    out = proc.stdout + proc.stderr
    tail = "\n".join(out.strip().splitlines()[-40:])
    flash = ""
    m = re.search(r"\bFLASH1:\s+\d+ B\s+\d+ \w+\s+([\d.]+%)", out) or \
        re.search(r"AXIM_FLASH1:\s+\d+ B\s+\d+ \w+\s+([\d.]+%)", out)
    if m:
        flash = m.group(1)

    hex_name = ""
    if proc.returncode == 0:
        hexes = glob.glob(os.path.join(REPO, "obj", "*_" + target + ".hex"))
        if hexes:
            hex_name = os.path.basename(max(hexes, key=os.path.getmtime))

    return {"ok": proc.returncode == 0, "cmd": shown, "log": tail, "flash": flash, "hex": hex_name}


PAGE = r"""<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>MyTAflight Build Tool</title>
<style>
 :root{--bg:#12151c;--panel:#1b2029;--line:#2c333f;--fg:#e6e9ef;--muted:#8b93a3;--accent:#4ea1ff;--ok:#3ad29f;--err:#ff5d5d;--warn:#ffb454;}
 *{box-sizing:border-box} body{margin:0;font:14px/1.45 -apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg)}
 header{padding:14px 18px;border-bottom:1px solid var(--line)} header h1{margin:0;font-size:16px} header p{margin:4px 0 0;color:var(--muted);font-size:12px}
 main{max-width:900px;margin:0 auto;padding:16px}
 .panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px;margin-bottom:16px}
 h2{margin:0 0 10px;font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.05em}
 label{font-size:13px} select,input[type=text]{background:#0c0f15;color:var(--fg);border:1px solid var(--line);border-radius:5px;padding:6px 8px;font:inherit}
 select:disabled{opacity:.5}
 .row{display:flex;gap:16px;align-items:center;flex-wrap:wrap}
 .field{display:flex;flex-direction:column;gap:4px;min-width:180px}
 .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:4px 14px}
 .chk{display:flex;gap:6px;align-items:center;color:var(--fg);font-size:13px;padding:2px 0}
 .chk input{accent-color:var(--accent)} .chk.disabled{opacity:.5}
 .muted{color:var(--muted);font-size:12px} .warn{color:var(--warn);font-size:12px}
 button{background:var(--accent);color:#04101f;border:0;border-radius:6px;padding:9px 16px;font-weight:700;cursor:pointer;font:inherit}
 button:disabled{opacity:.5;cursor:default}
 code{background:#0c0f15;border:1px solid var(--line);border-radius:5px;padding:2px 6px;font-size:12px}
 pre{background:#0a0d12;border:1px solid var(--line);border-radius:6px;padding:10px;overflow:auto;font-size:12px;max-height:320px;white-space:pre-wrap}
 .pill{display:inline-block;padding:2px 8px;border-radius:20px;font-size:12px;font-weight:700}
 .pill.ok{background:rgba(58,210,159,.15);color:var(--ok)} .pill.err{background:rgba(255,93,93,.15);color:var(--err)}
 a.dl{color:var(--ok);font-weight:700}
 input[type=text].wide{width:100%}
</style></head><body>
<header><h1>MyTAflight — Build Tool</h1><p>Modeled on Betaflight's own cloud-build page: pick a target, protocols and extras, build, download the <code>.hex</code>. Always builds <code>CLOUD_BUILD</code>-style (only what you pick is compiled in) via <code>make &lt;TARGET&gt; OPTIONS="…"</code>.</p></header>
<main>
 <div class="panel">
   <div class="row">
     <div class="field"><label>Target</label><select id="target"></select></div>
   </div>
 </div>

 <div class="panel">
   <h2>Radio protocol</h2>
   <div class="row">
     <div class="field"><select id="rx"></select></div>
   </div>
 </div>

 <div class="panel">
   <h2>Telemetry protocol</h2>
   <div class="row">
     <div class="field"><select id="tel"></select></div>
   </div>
   <p class="muted" id="telNote"></p>
 </div>

 <div class="panel">
   <h2>OSD / Video system</h2>
   <div class="row">
     <div class="field"><select id="osd"></select></div>
   </div>
 </div>

 <div class="panel">
   <h2>Motor protocol</h2>
   <div class="row">
     <div class="field"><select id="motor"></select></div>
   </div>
   <p class="muted">Only DShot changes the firmware build (it adds the DShot driver). Brushed/Multishot/Oneshot/Proshot/PWM all use the same always-present PWM driver — pick among them anytime via the <code>motor_pwm_protocol</code> CLI setting, no rebuild needed.</p>
 </div>

 <div class="panel"><h2>Custom features (this fork)</h2>
   <div class="grid" id="grp-custom"></div>
   <p class="muted">Battery impedance is always compiled in (unconditional in this fork) — not a toggle.</p>
 </div>

 <div class="panel"><h2>Other options</h2><div class="grid" id="grp-other"></div></div>

 <div class="panel"><h2>Custom defines</h2>
   <input type="text" class="wide" id="customDefines" placeholder="e.g. USE_SERVOS SOME_FLAG=5">
   <p class="muted">Extra <code>-D</code> defines to pass straight through, space-separated. Same character rules as everything else (letters/digits/underscore, optional <code>=value</code>).</p>
   <p class="warn" id="customWarn" style="display:none"></p>
 </div>

 <div class="panel" id="postFlashPanel" style="display:none">
   <h2>Post-flash CLI</h2>
   <p class="muted">Some choices here are runtime settings, not build flags — set them once after flashing:</p>
   <pre id="postFlashCli"></pre>
 </div>

 <div class="panel">
   <div class="row"><button id="buildBtn">Build firmware</button>
     <span class="muted">will run:</span> <code id="cmdPreview">make …</code></div>
 </div>

 <div class="panel" id="resultPanel" style="display:none">
   <h2>Result</h2>
   <div class="row" style="margin-bottom:8px"><span id="status" class="pill"></span>
     <span id="flash" class="muted"></span> <span id="dl"></span></div>
   <pre id="log"></pre>
 </div>
</main>
<script>
const TARGETS = __TARGETS__;
const ALLOWED = /^[A-Z0-9_]+(=[A-Za-z0-9_.]+)?$/;

// ---- Radio protocol (single choice). Serial protocols additionally need the USE_SERIALRX
// umbrella (added automatically); PPM does not (it's a separate, non-serial input path). ----
const RX = [
  ["USE_SERIALRX_CRSF",     "CRSF (TBS Crossfire)"],
  ["USE_SERIALRX_FPORT",    "FPort"],
  ["USE_SERIALRX_GHST",     "Ghost (GHST)"],
  ["USE_SERIALRX_IBUS",     "IBUS"],
  ["USE_SERIALRX_JETIEXBUS","JetiExBus"],
  ["USE_SERIALRX_MAVLINK",  "MAVLink"],
  ["USE_RX_PPM",            "PPM"],
  ["USE_SERIALRX_SBUS",     "SBUS"],
  ["USE_SERIALRX_SPEKTRUM", "Spektrum (SRXL/DSM2/DSMX)"],
  ["USE_SERIALRX_SRXL2",    "SRXL2"],
  ["USE_SERIALRX_SUMD",     "SUMD"],
  ["USE_SERIALRX_SUMH",     "SUMH"],
  ["USE_SERIALRX_XBUS",     "XBUS"],
];
const RX_DEFAULT = "USE_SERIALRX_CRSF";

// RX protocols whose telemetry is bundled and must not be chosen separately: the actual
// telemetry flag(s) BF needs for each (note FPort rides on SmartPort framing; there is no
// separate USE_TELEMETRY_FPORT).
const AUTO_TELEMETRY = {
  "USE_SERIALRX_CRSF":      ["USE_TELEMETRY_CRSF"],
  "USE_SERIALRX_FPORT":     ["USE_TELEMETRY_SMARTPORT"],
  "USE_SERIALRX_GHST":      ["USE_TELEMETRY_GHST"],
  "USE_SERIALRX_JETIEXBUS": ["USE_TELEMETRY_JETIEXBUS"],
};

// ---- Telemetry protocol (single choice, disabled/auto when RX is one of the above). ----
const TEL = [
  ["",                          "— none —"],
  ["USE_TELEMETRY_FRSKY_HUB",   "FrSky Hub"],
  ["USE_TELEMETRY_HOTT",        "HoTT"],
  ["USE_TELEMETRY_IBUS_EXTENDED","IBUS (extended)"],
  ["USE_TELEMETRY_LTM",         "LTM"],
  ["USE_TELEMETRY_MAVLINK",     "MAVLink"],
  ["USE_TELEMETRY_SMARTPORT",   "SmartPort"],
  ["USE_TELEMETRY_SRXL",        "SRXL"],
];

// ---- OSD / video system (single choice). ----
const OSD = [
  ["",              "None"],
  ["USE_OSD_SD",    "Analog (MAX7456)"],
  ["USE_OSD_HD",    "Digital (DJI O3/O4, HDZero, Walksnail, WTF-OS)"],
  ["USE_FRSKYOSD",  "FrSky OSD"],
];
const OSD_DEFAULT = "USE_OSD_HD";

// ---- Motor protocol (single choice). Only DShot maps to a real build flag. ----
const MOTOR = [
  ["USE_DSHOT", "DShot"],
  ["",          "Brushed"],
  ["",          "Multishot"],
  ["",          "Oneshot"],
  ["",          "Proshot"],
  ["",          "PWM"],
];

// ---- This fork's own features. "Cruise" has no build flag of its own: it needs Position
// Hold compiled in (forced on below if checked) and is a runtime CLI setting after flashing. ----
const CUSTOM = [
  [["USE_TEMPERATURE_SENSOR"], "Temperature sensor (I2C)", false],
  [["USE_ADSB"],               "ADS-B traffic (needs GPS)", false],
  [["__CRUISE__"],             "Position-hold CRUISE (velocity-stick)", true],
];

// ---- Everything else BF supports, as independently toggleable checkboxes under CLOUD_BUILD.
// Each entry may expand to more than one actual flag (e.g. EMFAT, ESC Serial SimonK). ----
const OTHER = [
  [["USE_ACRO_TRAINER"], "Acro Trainer", false],
  [["USE_AKK_SMARTAUDIO"], "AKK SmartAudio (SA Fox quirk)", false],
  [["USE_ALTITUDE_HOLD"], "Altitude Hold", true],
  [["USE_BATTERY_CONTINUE"], "Batt. Continue", false],
  [["USE_CAMERA_CONTROL"], "Cam. Control", false],
  [["USE_CHIRP"], "Chirp (PID frequency sweep)", false],
  [["USE_DASHBOARD"], "DashBoard (OLED)", false],
  [["USE_EMFAT_AUTORUN", "USE_EMFAT_ICON"], "EMFAT (autorun icon)", false],
  [["USE_ESCSERIAL_SIMONK", "USE_SERIAL_4WAY_SK_BOOTLOADER"], "ESC Serial (SK) incl. 4way", false],
  [["USE_FLIGHT_PLAN"], "Flight Plan", false],
  [["USE_GPS"], "GPS", true],
  [["USE_LED_STRIP"], "LED Strip", false],
  [["USE_LED_STRIP_64"], "LED Strip (64)", false],
  [["USE_MAG"], "Magnetometers", true],
  [["USE_OPTICALFLOW"], "Optical Flow", false],
  [["USE_PINIO"], "Pin IO", false],
  [["USE_POSITION_HOLD"], "Position hold", true],
  [["USE_RACE_PRO"], "Race Pro", false],
  [["USE_RANGEFINDER"], "Range finder", false],
  [["USE_SOFTSERIAL"], "Soft Serial", false],
  [["USE_VTX"], "VTX", false],
  [["USE_WING"], "Wing", false],
];

const $=id=>document.getElementById(id);
$("target").innerHTML = TARGETS.map(t=>`<option>${t}</option>`).join("");

function fillSelect(id, list, defaultValue){
  $(id).innerHTML = list.map(([v,l])=>`<option value="${v}" ${v===defaultValue?"selected":""}>${l}</option>`).join("");
}
fillSelect("rx", RX, RX_DEFAULT);
fillSelect("tel", TEL, "");
fillSelect("osd", OSD, OSD_DEFAULT);
fillSelect("motor", MOTOR, "USE_DSHOT");

function fillChecks(id, list){
  $(id).innerHTML = list.map(([flags,label,def])=>
    `<label class="chk"><input type="checkbox" data-flags="${flags.join(",")}" ${def?"checked":""}> ${label}</label>`).join("");
}
fillChecks("grp-custom", CUSTOM);
fillChecks("grp-other", OTHER);

function checkedFlags(id){
  return [...document.querySelectorAll(`#${id} input:checked`)]
    .flatMap(i=>i.dataset.flags.split(","));
}

function updateTelemetryUI(){
  const rx = $("rx").value;
  const auto = AUTO_TELEMETRY[rx];
  $("tel").disabled = !!auto;
  if (auto){
    $("telNote").textContent = "Incluso automaticamente (bundled with the selected radio protocol).";
  } else {
    $("telNote").textContent = "";
  }
}

function computeOptions(){
  let opts = ["CLOUD_BUILD"];

  // Radio protocol
  const rx = $("rx").value;
  if (rx){
    opts.push(rx);
    if (rx !== "USE_RX_PPM") opts.push("USE_SERIALRX");
  }

  // Telemetry: bundled-and-forced for CRSF/FPort/Ghost/JetiExBus, otherwise the dropdown choice.
  const auto = AUTO_TELEMETRY[rx];
  if (auto){
    opts.push(...auto, "USE_TELEMETRY");
  } else {
    const tel = $("tel").value;
    if (tel) opts.push(tel, "USE_TELEMETRY");
  }

  // OSD
  const osd = $("osd").value;
  if (osd) opts.push(osd);

  // Motor protocol (only DShot is a real flag)
  const motor = $("motor").value;
  if (motor) opts.push(motor);

  // This fork's features
  const custom = checkedFlags("grp-custom");
  const cruiseChecked = custom.includes("__CRUISE__");
  opts.push(...custom.filter(f=>f!=="__CRUISE__"));

  // Other options
  opts.push(...checkedFlags("grp-other"));

  // Cruise needs Position Hold compiled in, regardless of that checkbox's own state.
  if (cruiseChecked) opts.push("USE_POSITION_HOLD");

  // ADS-B needs GPS on every board.
  if (opts.includes("USE_ADSB") && !opts.includes("USE_GPS")) opts.push("USE_GPS");

  // Position Hold (and so Cruise, which forces it above) needs a position source: GPS or
  // Optical Flow. Without either, common_post.h's #error stops the build.
  if (opts.includes("USE_POSITION_HOLD") && !opts.includes("USE_GPS") && !opts.includes("USE_OPTICALFLOW")) {
    opts.push("USE_GPS");
  }

  // Custom defines free text
  const customText = $("customDefines").value.trim();
  const invalid = [];
  if (customText){
    for (const tok of customText.split(/\s+/)){
      if (ALLOWED.test(tok)) opts.push(tok); else invalid.push(tok);
    }
  }
  $("customWarn").style.display = invalid.length ? "block" : "none";
  $("customWarn").textContent = invalid.length ? ("Ignored (doesn't look like a define): " + invalid.join(", ")) : "";

  return { opts: [...new Set(opts)], cruiseChecked };
}

function refresh(){
  updateTelemetryUI();
  const { opts, cruiseChecked } = computeOptions();
  $("cmdPreview").textContent = "make " + $("target").value + (opts.length? ` OPTIONS="${opts.join(" ")}"` : "");

  const postFlash = [];
  if (cruiseChecked) postFlash.push("set pos_hold_navmode = CRUISE");
  $("postFlashPanel").style.display = postFlash.length ? "block" : "none";
  $("postFlashCli").textContent = postFlash.join("\n");
}
document.addEventListener("change", refresh);
document.addEventListener("input", refresh);
refresh();

$("buildBtn").onclick = async () => {
  const btn = $("buildBtn"); btn.disabled = true; btn.textContent = "Building…";
  $("resultPanel").style.display = "block"; $("status").className="pill"; $("status").textContent="running";
  $("flash").textContent=""; $("dl").innerHTML=""; $("log").textContent="Compiling, please wait (can take ~30-120s)…";
  try {
    const { opts } = computeOptions();
    const r = await fetch("/build", {method:"POST", headers:{"Content-Type":"application/json"},
      body: JSON.stringify({target:$("target").value, options:opts})});
    const d = await r.json();
    $("status").className = "pill " + (d.ok?"ok":"err"); $("status").textContent = d.ok?"success":"failed";
    $("flash").textContent = d.flash ? ("flash "+d.flash) : "";
    $("log").textContent = (d.cmd?("$ "+d.cmd+"\n\n"):"") + d.log;
    $("dl").innerHTML = (d.ok && d.hex) ? `<a class="dl" href="/hex/${encodeURIComponent(d.hex)}" download>⤓ ${d.hex}</a>` : "";
  } catch(e){ $("status").className="pill err"; $("status").textContent="error"; $("log").textContent=String(e); }
  btn.disabled=false; btn.textContent="Build firmware";
};
</script></body></html>"""


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="text/html; charset=utf-8", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        if extra:
            for k, v in extra.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body if isinstance(body, bytes) else body.encode())

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            self._send(200, PAGE.replace("__TARGETS__", json.dumps(TARGETS)))
        elif self.path.startswith("/hex/"):
            name = os.path.basename(self.path[len("/hex/"):])
            path = os.path.join(REPO, "obj", name)
            if name.endswith(".hex") and os.path.isfile(path):
                with open(path, "rb") as f:
                    self._send(200, f.read(), "application/octet-stream",
                               {"Content-Disposition": f'attachment; filename="{name}"'})
            else:
                self._send(404, "not found", "text/plain")
        else:
            self._send(404, "not found", "text/plain")

    def do_POST(self):
        if self.path != "/build":
            self._send(404, "not found", "text/plain")
            return
        length = int(self.headers.get("Content-Length", 0))
        try:
            req = json.loads(self.rfile.read(length) or b"{}")
        except Exception:
            req = {}
        result = run_build(req.get("target", ""), req.get("options", []) or [])
        self._send(200, json.dumps(result), "application/json")


if __name__ == "__main__":
    print(f"MyTAflight build tool — repo: {REPO}")
    print(f"Open http://localhost:{PORT}")
    http.server.HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
