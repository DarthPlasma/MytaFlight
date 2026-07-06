# MyTAflight — project handoff

Personal fork of **Betaflight** adding four custom features (ported/adapted from iNAV where sensible).
This document is a self-contained handoff so the project can be resumed from another device / a fresh chat.

Base: Betaflight **master** (calendar-versioned `2026.6.0-alpha`), GPLv3. Cloned ~2026-07-03, base commit `14e6e1b`.
Everything below was developed 2026-07-03 → 2026-07-05.

---

## 1. Quick start on a new machine

```bash
# 1. clone your fork
git clone https://github.com/DarthPlasma/MytaFlight.git
cd MytaFlight
git remote add upstream https://github.com/betaflight/betaflight.git

# 2. toolchain (macOS/Linux/WSL2). Apple 'make' 3.81 is fine; no Homebrew needed on macOS
make arm_sdk_install          # installs ARM GNU GCC 13.3.1 into tools/
make targets                  # list targets

# 3. build a full firmware with all opt-in features for a target
git checkout integration
make KAKUTEH7 OPTIONS="USE_TEMPERATURE_SENSOR USE_ADSB"     # H7 boards
# on boards whose config has no GPS by default (e.g. F722 Velox) add USE_GPS:
make TMOTORVELOXF7V2 OPTIONS="USE_TEMPERATURE_SENSOR USE_ADSB USE_GPS"

# output: obj/betaflight_2026.6.0-alpha_<TARGET>.hex  -> flash via Configurator "Load Firmware [Local]"
```

> The clone from GitHub is **shallow-safe** but if you cloned `--depth 1` you must run `git fetch --unshallow` before `git rebase upstream/master`.

---

## 2. GitHub / repo

- **Fork**: https://github.com/DarthPlasma/MytaFlight (GitHub user **DarthPlasma**). Public fork of `betaflight/betaflight`.
- **Remotes**: `origin` = the fork (push here), `upstream` = `betaflight/betaflight` (pull/rebase from here).
- **Auth**: a GitHub **Personal Access Token** (scope `repo`) stored in the macOS **osxkeychain** helper. Git push does NOT use the account password (GitHub disabled that). On a new machine: `gh auth login` (browser) or a PAT in the git credential prompt.
- Keep feature branches rebased forward on `upstream/master` periodically (small, frequent rebases). The features are gated/additive by design, so rebases stay cheap.

### Branches (all pushed to the fork)
| Branch | Contents |
|--------|----------|
| `master` | pristine upstream base (14e6e1b) |
| `feature/battery-impedance` | Feature A (battery impedance) |
| `feature/temp-sensors` | Feature B (I2C temperature) + both tools live here historically |
| `feature/adsb` | Feature C (ADS-B) |
| `feature/poshold-vector` | Feature D (CRUISE velocity-stick pos-hold) — **NOT flight-validated** |
| **`integration`** | merges A+B+C+**D** + tools (build-tool, osd-layout, taranis-sitl-rc, SITL-GAZEBO.md). The branch to flash. Feature D merged 2026-07-05 (commit f8ac642) with default `ANGLE`, **field-test pending** — SITL could not validate it. |

Feature branches are independent (each from master) for clean upstream rebasing. `integration` is where they come together.

---

## 3. Build system essentials

- Targets use the MCU/board name: `make KAKUTEH7` (NOT `make TARGET_...`). Board configs are hydrated in `src/config/configs/<BOARD>/config.h` (618 boards; `make configs` to re-hydrate).
- **Three build modes** (see `src/main/target/common_pre.h`):
  - default `make <TARGET>` = full build, almost everything on.
  - `OPTIONS="CLOUD_BUILD ..."` = only listed features (smaller). **Gotcha:** most flight features (DSHOT, RPM/dyn-notch filters, pos/alt hold, LED, VTX, blackbox, GPS-rescue, etc.) AND `USE_BATTERY_IMPEDANCE` are defined **unconditionally by the platform header / common_pre.h** — they are always compiled and passing them via `-D` errors with `"redefined"`. Only genuinely cloud-selectable flags may be passed: `USE_OSD_SD/HD`, `USE_GPS`, `USE_SERIALRX_*` / `USE_RX_EXPRESSLRS`, `USE_TELEMETRY_*`, and the opt-in customs `USE_TEMPERATURE_SENSOR`/`USE_ADSB`. Those flight features are configured at RUNTIME in the Configurator, not at build time.
  - `OPTIONS="CORE_BUILD"` = minimal features, all drivers (rarely useful here).
- **SITL** (software-in-the-loop): `make SITL` → `obj/betaflight_..._SITL`. Runs on the PC, connects a simulator over UDP (Gazebo :9002 / RealFlight :9001, state :9003, RC :9004). Used to validate Feature D.

### Confirmed hardware targets & baseline flash (H7 = huge headroom)
| Board | `make` target | MCU | Notes |
|-------|---------------|-----|-------|
| DAKEFPV H743 | `DAKEFPVH743` | H743 | dev/primary, ~31% flash |
| T-Motor Velox F7 V2 | `TMOTORVELOXF7V2` | F722 | the constrained one (~77% baseline, ~85% with all features + GPS; use CLOUD_BUILD to trim) |
| HolyBro Kakute H7 V1.3 **and** V1.5 | `KAKUTEH7` | H743 | one config auto-detects the gyro (MPU6000/ICM42688P/BMI270) |
| Diatone MAMBA H743 Mark 4 | `MAMBAH743_2022B` | H743 | likely MK4 (gyro ICM42688P); older original = `MAMBAH743` |

---

## 4. The four features

### A — Battery internal impedance  (branch `feature/battery-impedance`, gate `USE_BATTERY_IMPEDANCE`, **default ON**)
Estimates battery internal resistance (mΩ) via opportunistic ΔV/ΔI (iNAV method — passive, no current-interrupt), plus a current-aware sag-compensated voltage. Runs from the 50 Hz battery-current task.
- Files: `sensors/battery.{c,h}` (est. logic, getters, config), `osd/osd.h` + `osd/osd_elements.c` (2 OSD elements), `cli/settings.c`, `target/common_pre.h` (the flag).
- CLI: `battery_impedance_i_threshold` (200 = 2A), `battery_impedance_v_threshold` (4 = 40mV), `battery_impedance_lpf_period` (12 = 1.2s), `battery_impedance_stable_count` (10).
- OSD: `OSD_BATTERY_IMPEDANCE` ("<n>mR", no ohm glyph in BF font), `OSD_SAG_COMP_BATT_VOLTAGE`. CLI pos: `osd_battery_impedance_pos`, `osd_sag_comp_batt_pos`.
- Note: BF already has a *voltage-only LPF* sag comp for the mixer — we did NOT touch it; impedance is additive. Won't converge on the bench (needs in-flight throttle steps).

### B — I2C temperature sensor (LM75) → OSD  (branch `feature/temp-sensors`, gate `USE_TEMPERATURE_SENSOR`, **opt-in / default OFF**)
- Files: `drivers/temperature/lm75.{c,h}` (reg 0x00, iNAV conversion → 0.1°C), `sensors/temperature.{c,h}` (module + config + getters), wired into `scheduler.h`/`fc/tasks.c` (TASK_TEMPERATURE 2Hz) + `sensors/initialisation.c`, OSD element in `osd_elements.c`, `cli/settings.c`, `pg/pg_ids.h`.
- CLI: `temp_sensor_i2c_device`, `temp_sensor_i2c_address` (0x48=72), `temp_sensor_alarm_min` (0), `temp_sensor_alarm_max` (60).
- OSD: `OSD_BATTERY_TEMPERATURE` renders `B[thermo]<t>C`, dashes when absent, **blinks** (via `osdUpdateAlarms`) when `!(alarm_min < T < alarm_max)`. CLI pos: `osd_battery_temp_pos`.

### C — ADS-B via MAVLink → OSD  (branch `feature/adsb`, gate `USE_ADSB`, **opt-in**, requires `USE_GPS`)
The handoff doc was wrong that "BF has only MAVLink TX": modern BF bundles the full MAVLink lib (`lib/main/MAVLink/`) incl. `mavlink_msg_adsb_vehicle.h`, and already parses incoming MAVLink in `telemetry/mavlink.c` (`mavlinkDispatch()`). So we only added a module + hook.
- Files: `io/adsb.{c,h}` (vehicle list, distance/bearing via `GPS_distance_cm_bearing`, TTL, threat detection; adapted from iNAV), hook `case MAVLINK_MSG_ID_ADSB_VEHICLE` in `telemetry/mavlink.c`, OSD elements in `osd_elements.c`, `osd/osd_warnings.c` (critical warning), `pg/pg_ids.h` (PG_ADSB_CONFIG 2048), `cli/settings.c`, `mk/source.mk`.
- CLI: `adsb_max_dist_horiz` (m, 50000), `adsb_max_dist_vert` (m, 2000; asymmetric — hides only traffic ABOVE us beyond this, below is always shown), `adsb_detection_cone` (centideg, 2000 = ±10°), `adsb_aircraft_toa` (s, 60). Own altitude uses the FUSED estimate (`GPS_home_llh.altCm + getEstimatedAltitudeCm()`).
- OSD: `OSD_ADSB_WARNING` (arrow-to-traffic + distance + Δalt), `OSD_ADSB_INFO` (movement arrow + class + speed + callsign), `OSD_ADSB_STATUS` ("A<detected>/<in-range>"). CLI pos: `osd_adsb_warning_pos`, `osd_adsb_info_pos`, `osd_adsb_status_pos`.
- Critical OSD system message **"AIRCRAFT APPROACHING"** (blinking, after FAIL SAFE) when an aircraft heads toward us within the cone, ToA ≤ threshold, and within the vertical envelope. Callsign is from the frame (blanked when the VALID_CALLSIGN flag is absent).

### D — Vector "cruise" position hold  (branch `feature/poshold-vector`, config `pos_hold_navmode`, **NOT flight-validated**)
Key finding: modern BF master **already has** a Kalman position/velocity estimator (`flight/position_estimator.c`) and a vector pos→vel→accel→angle cascade (`flight/autopilot_multirotor.c::positionControl()`) — the "vector position hold" the old doc wanted is already upstream. The genuine gap: on stick input, BF pos-hold **hands over to raw pilot angle mode** (drift in crosswind, false sense of control). iNAV instead commands velocity.
- What we added: CLI `pos_hold_navmode` = `ANGLE` (default, classic) | `CRUISE`. In CRUISE, inside POS HOLD, pitch/roll stick deflection commands a wind-compensated **velocity** (magnitude `ap_max_velocity`, shared with waypoint nav) instead of angle mode; yaw stays pilot; releasing brakes to a hold.
- Files: `pg/pos_hold_multirotor.{c,h}` (navMode + enum, PG v2→3), `cli/settings.{c,h}` (lookup ANGLE/CRUISE), `flight/autopilot_multirotor.c` (`cruiseVelocityFromSticks()`, wire into `positionControl()`, keep `isAutopilotInControl()` true in CRUISE). Guarded by `FLIGHT_MODE(POS_HOLD_MODE)` at the pid.c call site; default ANGLE = zero behaviour change.
- **Status (2026-07-05): merged to `integration` (f8ac642); NOT flight-validated.** Earth-frame math validated offline (native C test, 13/13). SITL could **not** validate it: getting a quad to fly pos-hold in the SITL+Gazebo setup needed a cascade of integration fixes (pos/alt hold not compiled into SITL → `target.h`; heading invalid at hover → `imu.c` `imuIsHeadingValid()` returns true under `SIMULATOR_BUILD`; both SITL-only, commit 7d7a594), and finally hit an unresolved **yaw↔position frame mismatch causing toilet-bowl** in pos hold (affects all pos hold in that sim setup, not the feature). Decision: field-test on real hardware (DAKEFPVH743) with acro fallback.
- **TODO at field test**: confirm stick-sign convention (pitch-forward → forward, not backward; roll direction), brake-to-hold on release, failsafe/sensor-loss. If a sign is inverted, flip it in `cruiseVelocityFromSticks()`. Default stays ANGLE so flashing changes nothing until CRUISE is selected.
- SITL tooling (branch `integration`, `mytaflight-tools/`): `taranis-sitl-rc.py` (EdgeTX Taranis USB joystick → SITL UDP :9004 RC bridge, runs on the Mac with `--host <VM_IP>`), `SITL-GAZEBO.md` (full UTM+Ubuntu 24.04+Gazebo Harmonic pipeline).

---

## 5. Support tools (in `mytaflight-tools/`, on `integration`)

### `osd-layout.html` — OSD positioning helper (single-file HTML/JS, open in a browser)
Betaflight's stock Configurator doesn't know our new OSD element IDs, so they're positioned via CLI (`set osd_*_pos = <encoded>`). This tool: paste a CLI `diff`/`dump`, arrange elements on a **video-system-aware grid** (Analog PAL/NTSC, HDZero, DJI O3/O4, **WTF-OS 60×22**, Walksnail, Custom), WYSIWYG with realistic inline-SVG glyphs, background-image overlay + dim, 3 OSD profiles, click-to-place, and copy the `set osd_*_pos` lines back. Encoding taken authoritatively from `osd.h` (X is 6-bit incl. the bit-10 HD extension; profile bits 11-13; type bits 14-15 preserved). Element names validated against real BF (`osd_ah_pos`, `osd_ah_sbar_pos`, etc.).

### `build-tool.py` — local firmware build UI (single-file stdlib Python)
`python3 mytaflight-tools/build-tool.py` → http://localhost:8792. Target dropdown (the 5 boards), **Full** (everything + opt-in extras) vs **Slim** (CLOUD_BUILD, only ticked flags), custom features / sensors / receiver (radio) / telemetry (radio) / OSD SD+HD (checkboxes). Runs `make`, reports pass/fail + FLASH1/AXIM_FLASH1 %, and serves the `.hex` for download. Full mode auto-adds USE_GPS for ADS-B. Slim only passes genuinely-selectable flags (see §3 gotcha).

---

## 6. Directory tree (project-specific additions/changes)

```
MytaFlight/
├── MYTAFLIGHT.md                      # this file
├── mytaflight-tools/
│   ├── osd-layout.html               # OSD positioning WYSIWYG tool
│   └── build-tool.py                 # local firmware build UI
├── src/main/
│   ├── drivers/temperature/
│   │   └── lm75.{c,h}                 # [B] LM75 I2C driver
│   ├── io/
│   │   └── adsb.{c,h}                 # [C] ADS-B vehicle tracking module
│   ├── sensors/
│   │   ├── battery.{c,h}             # [A] impedance + sag-comp (modified)
│   │   └── temperature.{c,h}         # [B] temperature sensor module
│   ├── flight/
│   │   ├── autopilot_multirotor.c    # [D] CRUISE control law (modified)
│   │   └── position_estimator.c      # (BF's Kalman estimator — reference)
│   ├── pg/
│   │   ├── pos_hold_multirotor.{c,h} # [D] navMode config (modified)
│   │   └── pg_ids.h                  # [B,C] new PG ids (modified)
│   ├── osd/
│   │   ├── osd.h / osd_elements.c    # [A,B,C] new OSD elements (modified)
│   │   └── osd_warnings.c            # [C] AIRCRAFT APPROACHING (modified)
│   ├── telemetry/mavlink.c           # [C] ADSB_VEHICLE hook (modified)
│   ├── cli/settings.{c,h}            # [A,B,C,D] CLI params (modified)
│   ├── scheduler/scheduler.h         # [B] TASK_TEMPERATURE (modified)
│   ├── fc/tasks.c                    # [B] task registration (modified)
│   ├── sensors/initialisation.c     # [B] sensor init (modified)
│   ├── target/common_pre.h          # [A] USE_BATTERY_IMPEDANCE flag (modified)
│   └── mk → mk/source.mk            # [B,C] compile new .c files (modified)
└── (… full unmodified Betaflight tree …)
```
`.claude/launch.json` (gitignored) defines a static-server preview for the tools. `tools/` holds the ARM SDK (gitignored). `obj/` holds build output (gitignored).

---

## 7. Key learnings / gotchas (don't relearn these)
- Adding a field to a PG struct does **not** expose it to CLI — you also need a `valueTable[]` entry in `cli/settings.c` (and, for OSD element positions, an `item_pos[...]` entry). Lookup enums live in `cli/settings.h` and must stay in the same order as the `lookupTables[]` array in `settings.c`.
- New OSD element = enum in `osd.h` + draw fn + entry in `osdElementDisplayOrder[]` (else it isn't drawn) + `osdElementDrawFunction[]` + a `osd_*_pos` CLI setting. The stock Configurator won't list new IDs → position via CLI (hence the OSD tool).
- BF units: amperage/voltage in centi-units (0.01A / 0.01V); `gpsLocation_t` altitude field is `altCm` (cm); OSD position X is 6-bit (bits 0-4 + bit 10 for HD cols 32-63).
- `USE_ADSB` requires `USE_GPS` (enforced with `#error`). Some board configs (F722 Velox) don't enable GPS by default.
- Most flight features are platform-forced and configured at runtime, not build-time (see §3). BF is migrating `USE_` → `ENABLE_` (e.g. `ENABLE_FLIGHT_PLAN=0` can disable flight plan at build; most still can't).
- LTO "type mismatch" warnings after changing a struct/enum size are stale-incremental artifacts → `make clean` clears them.

## 8. Open items / next steps
1. **Feature D**: merged to `integration` (SITL validation abandoned — frame/toilet-bowl issues). **Field-test CRUISE on DAKEFPVH743**: confirm stick signs + brake-to-hold; flip a sign in `cruiseVelocityFromSticks()` if pitch/roll inverted. Optionally clamp diagonal velocity to `ap_max_velocity` (currently per-axis, ~√2 in diagonal — accepted).
2. Periodically `git fetch upstream && git rebase upstream/master` each feature branch (do `git fetch --unshallow` first if shallow).
3. Optional tool ideas: `ENABLE_X=0` toggles in the build tool for the few build-disableable features; pixel-accurate `.mcm` font in the OSD tool.
