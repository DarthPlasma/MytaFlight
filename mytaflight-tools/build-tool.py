#!/usr/bin/env python3
"""
MyTAflight build tool.

A tiny local web UI to build firmware for the project's targets. Pick a target,
the features / receiver / telemetry / OSD you want, hit Build, and download the
resulting .hex. It just drives `make <TARGET> OPTIONS="..."` in the repo.

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

# Whitelist of build flags the UI may pass (guards the make invocation).
ALLOWED = re.compile(r"^[A-Z0-9_]+$")


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
 :root{--bg:#12151c;--panel:#1b2029;--line:#2c333f;--fg:#e6e9ef;--muted:#8b93a3;--accent:#4ea1ff;--ok:#3ad29f;--err:#ff5d5d;}
 *{box-sizing:border-box} body{margin:0;font:14px/1.45 -apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg)}
 header{padding:14px 18px;border-bottom:1px solid var(--line)} header h1{margin:0;font-size:16px} header p{margin:4px 0 0;color:var(--muted);font-size:12px}
 main{max-width:900px;margin:0 auto;padding:16px}
 .panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px;margin-bottom:16px}
 h2{margin:0 0 10px;font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.05em}
 label{font-size:13px} select{background:#0c0f15;color:var(--fg);border:1px solid var(--line);border-radius:5px;padding:6px 8px;font:inherit}
 .row{display:flex;gap:16px;align-items:center;flex-wrap:wrap}
 .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:4px 14px}
 .chk{display:flex;gap:6px;align-items:center;color:var(--fg);font-size:13px;padding:2px 0}
 .chk input{accent-color:var(--accent)} .muted{color:var(--muted);font-size:12px}
 button{background:var(--accent);color:#04101f;border:0;border-radius:6px;padding:9px 16px;font-weight:700;cursor:pointer;font:inherit}
 button:disabled{opacity:.5;cursor:default}
 code{background:#0c0f15;border:1px solid var(--line);border-radius:5px;padding:2px 6px;font-size:12px}
 pre{background:#0a0d12;border:1px solid var(--line);border-radius:6px;padding:10px;overflow:auto;font-size:12px;max-height:320px;white-space:pre-wrap}
 .pill{display:inline-block;padding:2px 8px;border-radius:20px;font-size:12px;font-weight:700}
 .pill.ok{background:rgba(58,210,159,.15);color:var(--ok)} .pill.err{background:rgba(255,93,93,.15);color:var(--err)}
 a.dl{color:var(--ok);font-weight:700} fieldset{border:1px solid var(--line);border-radius:6px;margin:0 0 10px;padding:8px 12px}
 legend{color:var(--muted);font-size:12px;padding:0 6px} input[type=radio]{accent-color:var(--accent)}
</style></head><body>
<header><h1>MyTAflight — Build Tool</h1><p>Pick a target and features, build, download the <code>.hex</code>. Drives <code>make &lt;TARGET&gt; OPTIONS="…"</code>.</p></header>
<main>
 <div class="panel">
   <div class="row">
     <label>Target &nbsp;<select id="target"></select></label>
     <fieldset style="margin:0"><legend>Build mode</legend>
       <label class="chk"><input type="radio" name="mode" value="full" checked> Full (everything on + extras — safe)</label>
       <label class="chk"><input type="radio" name="mode" value="slim"> Slim (CLOUD_BUILD — only what you tick; smaller, advanced)</label>
     </fieldset>
   </div>
   <p class="muted" id="modeNote"></p>
 </div>

 <div class="panel"><h2>Custom features (this fork)</h2><div class="grid" id="grp-custom"></div></div>
 <div class="panel slimonly"><h2>Sensors</h2><div class="grid" id="grp-sensor"></div></div>
 <div class="panel slimonly"><h2>Receiver (single)</h2><div class="grid" id="grp-rx"></div></div>
 <div class="panel slimonly"><h2>Telemetry (single)</h2><div class="grid" id="grp-tel"></div></div>
 <div class="panel slimonly"><h2>Video / OSD systems</h2><div class="grid" id="grp-osd"></div></div>
 <div class="panel slimonly"><p class="muted">Flight modes (pos/alt hold, GPS rescue, launch, acro trainer…), filters (RPM, dynamic notch), LED strip, VTX control, blackbox, camera control, servos, ESC sensor etc. are <b>always compiled in</b> on these STM32 targets and are enabled/configured at runtime in the Configurator — they are not build-time options.</p></div>

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
const CUSTOM = [["USE_BATTERY_IMPEDANCE","Battery impedance (default-on)",true],
                ["USE_TEMPERATURE_SENSOR","I2C temperature sensor",false],
                ["USE_ADSB","ADS-B traffic (needs GPS)",false]];
const SENSOR = [["USE_GPS","GPS",true]]; // baro/mag come from the board config; GPS is genuinely opt-in
const OSD = [["USE_OSD_HD","HD (DJI / Walksnail / HDZero)",true],["USE_OSD_HD","WTF-OS (DJI — same HD flag)",false],
             ["USE_OSD_SD","SD (analog MAX7456)",false]];
// Receiver and telemetry are single-choice (radio).
const RX = [["USE_SERIALRX_CRSF","Crossfire (CRSF)"],["USE_RX_EXPRESSLRS","ExpressLRS (SPI)"],["USE_SERIALRX_GHST","Ghost (GHST)"],
            ["USE_SERIALRX_SBUS","SBUS"],["USE_SERIALRX_SPEKTRUM","Spektrum"],["USE_SERIALRX_IBUS","IBUS"],
            ["USE_SERIALRX_FPORT","FPort"],["USE_SERIALRX_SRXL2","SRXL2"],["USE_SERIALRX_SUMD","SUMD"],["USE_SERIALRX_XBUS","XBUS"]];
const TEL = [["","— none —"],["USE_TELEMETRY_CRSF","Crossfire"],["USE_TELEMETRY_GHST","Ghost"],
             ["USE_TELEMETRY_SMARTPORT","SmartPort (FrSky)"],["USE_TELEMETRY_FRSKY_HUB","FrSky Hub"],
             ["USE_TELEMETRY_MAVLINK","MAVLink"],["USE_TELEMETRY_SRXL","SRXL"],["USE_TELEMETRY_IBUS","IBUS"],
             ["USE_TELEMETRY_LTM","LTM"],["USE_TELEMETRY_HOTT","HoTT"]];
const $=id=>document.getElementById(id);
$("target").innerHTML = TARGETS.map(t=>`<option>${t}</option>`).join("");
function fill(id,list){ $(id).innerHTML = list.map(([f,l,c])=>
  `<label class="chk"><input type="checkbox" value="${f}" ${c?"checked":""}> ${l}</label>`).join(""); }
function fillRadio(id,name,list,def){ $(id).innerHTML = list.map(([f,l],i)=>
  `<label class="chk"><input type="radio" name="${name}" value="${f}" ${i===def?"checked":""}> ${l}</label>`).join(""); }
fill("grp-custom",CUSTOM); fill("grp-sensor",SENSOR); fill("grp-osd",OSD);
fillRadio("grp-rx","rx",RX,0); fillRadio("grp-tel","tel",TEL,1);

function mode(){ return document.querySelector('input[name=mode]:checked').value; }
function checked(id){ return [...document.querySelectorAll(`#${id} input:checked`)].map(i=>i.value); }
function radioVal(name){ const el = document.querySelector(`input[name=${name}]:checked`); return el ? el.value : ""; }
function computeOptions(){
  const m = mode(); let opts = [];
  const custom = checked("grp-custom");
  if (m === "full") {
    // full build already has RX/telemetry/OSD/sensors/modes; only add opt-in extras
    opts = custom.filter(f=>f!=="USE_BATTERY_IMPEDANCE"); // impedance is default-on in full builds
    if (opts.includes("USE_ADSB") && !opts.includes("USE_GPS")) opts.push("USE_GPS"); // adsb needs gps on all boards
  } else {
    // impedance is unconditionally defined (always on), so it must not be passed here either
    opts = ["CLOUD_BUILD", ...checked("grp-osd"), ...checked("grp-sensor"),
            ...custom.filter(f=>f!=="USE_BATTERY_IMPEDANCE")];
    const rx = radioVal("rx"), tel = radioVal("tel");
    if (rx) opts.push(rx);
    if (tel) opts.push(tel);
    if (opts.includes("USE_ADSB") && !opts.includes("USE_GPS")) opts.push("USE_GPS");
  }
  return [...new Set(opts)];
}
function refresh(){
  const slim = mode()==="slim";
  document.querySelectorAll(".slimonly").forEach(e=>e.style.display = slim?"block":"none");
  $("modeNote").textContent = slim
    ? "Slim: only the ticked features are compiled in. You must tick a receiver and an OSD type or the firmware won't be usable."
    : "Full: all receivers, telemetry and both OSD types are already included; just choose the extra features to add.";
  const o = computeOptions();
  $("cmdPreview").textContent = "make " + $("target").value + (o.length? ` OPTIONS="${o.join(" ")}"` : "");
}
document.addEventListener("change", refresh); document.addEventListener("input", refresh); refresh();

$("buildBtn").onclick = async () => {
  const btn = $("buildBtn"); btn.disabled = true; btn.textContent = "Building…";
  $("resultPanel").style.display = "block"; $("status").className="pill"; $("status").textContent="running";
  $("flash").textContent=""; $("dl").innerHTML=""; $("log").textContent="Compiling, please wait (can take ~30-120s)…";
  try {
    const r = await fetch("/build", {method:"POST", headers:{"Content-Type":"application/json"},
      body: JSON.stringify({target:$("target").value, options:computeOptions()})});
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
