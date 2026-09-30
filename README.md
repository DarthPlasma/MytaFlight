# MyTAflight

MyTAflight is a **personal fork of [Betaflight](https://github.com/betaflight/betaflight)** that adds four traffic-safety / telemetry features (ported or adapted from iNAV) plus browser-based tooling. Every addition is **feature-gated and additive**: build with the custom flags off and you get stock Betaflight behaviour.

- **Base:** Betaflight **`2026.6.1` (stable)** · **Licence:** GPLv3
- **Flash this branch:** `integration` merges all four features + the tools.
- **Developer deep-dive:** see **[MYTAFLIGHT.md](MYTAFLIGHT.md)** for file-by-file changes, design notes and gotchas.

## What's added

|   | Feature | Build flag | Default | Requires |
|---|---------|-----------|---------|----------|
| **A** | Battery internal impedance + sag-compensated voltage | `USE_BATTERY_IMPEDANCE` | **on** | current meter |
| **B** | I²C temperature sensor (LM75) on the OSD | `USE_TEMPERATURE_SENSOR` | opt-in | LM75 sensor |
| **C** | ADS-B traffic awareness on the OSD | `USE_ADSB` | opt-in | GPS + MAVLink ADS-B RX |
| ~~D~~ | ~~Vector "cruise" position hold~~ | — | **superseded: stock in BF 2026.6.1** | GPS |

Build a firmware with the opt-in features:

```bash
git checkout integration
make KAKUTEH7 OPTIONS="USE_TEMPERATURE_SENSOR USE_ADSB"                   # H7 boards
make TMOTORVELOXF7V2 OPTIONS="USE_TEMPERATURE_SENSOR USE_ADSB USE_GPS"    # add USE_GPS where the config lacks it
```

Impedance (A) is always compiled in and controlled at runtime. Prefer a UI? Use the point-and-click [build tool](#tools).

---

## A — Battery internal impedance

Estimates the pack's **internal resistance in milliohms** and a **sag-compensated (no-load) voltage** using iNAV's passive ΔV/ΔI method (no current-interrupt). Whenever the current changes by a meaningful step it derives R = ΔV/ΔI from opportunistic samples, rejects outliers with a **5-sample median** filter, then smooths with a slow **PT1**.

| CLI setting | Default | Meaning |
|-------------|---------|---------|
| `battery_impedance_i_threshold` | `500` | min current step to take a sample (5.00 A) |
| `battery_impedance_v_threshold` | `10` | min voltage drop to take a sample (0.10 V) |
| `battery_impedance_lpf_period` | `60` | PT1 smoothing period (6.0 s) |
| `battery_impedance_stable_count` | `10` | stable samples required before a reading is trusted |

- **OSD:** `OSD_BATTERY_IMPEDANCE` shows the resistance in ohms as `<x>.<mmm>R` (e.g. `0.014R` = 14 mΩ; `R` stands in for the missing ohm glyph); `OSD_SAG_COMP_BATT_VOLTAGE` shows the no-load voltage. Place with `osd_battery_impedance_pos` / `osd_sag_comp_batt_pos`.
- **Note:** impedance only converges **in flight** (it needs real throttle steps) — it won't settle on the bench. It's additive to BF's existing voltage-only sag compensation, which is left untouched.

---

## B — I²C temperature sensor (LM75)

Reads an external **LM75 I²C temperature sensor** (e.g. taped to the battery) at 2 Hz and puts it on the OSD — useful for watching pack temperature on long-range / heavy builds. Enable with `USE_TEMPERATURE_SENSOR`.

- **CLI:** `temp_sensor_i2c_device`, `temp_sensor_i2c_address` (LM75 default `0x48` = 72), `temp_sensor_alarm_min` (0 °C), `temp_sensor_alarm_max` (60 °C).
- **OSD:** `OSD_BATTERY_TEMPERATURE` renders `B🌡<t>C` (dashes when the sensor is absent) and **blinks** when the temperature leaves the alarm window. Place with `osd_battery_temp_pos`.

---

## C — ADS-B traffic awareness ✈️

The headline feature. With a **MAVLink ADS-B receiver** (e.g. uAvionix pingRX, Aerobit TT-SC1) on a spare UART in MAVLink mode, the FC ingests `ADSB_VEHICLE` messages and turns nearby crewed-aircraft traffic into **OSD situational awareness and collision alerts**. Requires `USE_ADSB` and a GPS fix (for range/bearing). Up to `adsb_max_vehicle` aircraft (5–12, default 5) are tracked at once; distance/bearing come from your GPS position, vertical separation from your fused altitude.

### Configuration

| CLI setting | Default | Meaning |
|-------------|---------|---------|
| `adsb_max_dist_horiz` | `50000` | max horizontal distance to **display** (m) |
| `adsb_max_dist_vert` | `2000` | max height **above** you to display (m); traffic below you is always shown |
| `adsb_detection_cone` | `2000` | approach cone for the collision alert (centidegrees; 2000 = ±10°) |
| `adsb_aircraft_toa` | `60` | time-to-arrival threshold for the alert (s) |
| `adsb_max_vehicle` | `5` | aircraft tracked simultaneously (5–12) |

### Threat logic

An aircraft becomes a **critical threat** when it is **(a)** heading at you — its course is within ±½·cone of the reciprocal bearing, **(b)** closing within `adsb_aircraft_toa` seconds (distance ÷ ground speed), and **(c)** inside the display envelope. If several qualify, the one with the **lowest time-to-arrival** (most imminent) is shown.

### OSD elements

**Informational (nearest traffic):**
- `OSD_ADSB_WARNING` — arrow to the traffic + distance + altitude delta
- `OSD_ADSB_INFO` — movement arrow, aircraft class, speed, callsign
- `OSD_ADSB_STATUS` — `A<detected>/<in-range>`: **detected** = everything received over MAVLink (any distance, even with no GPS fix); **in-range** = only those within the distance/height limits

**Dedicated safety elements** — shown only during a critical threat, and independent of BF's generic warnings so they can't be pre-empted or overwrite neighbouring elements:
- `OSD_ADSB_CRITICAL_WARNING` — a steady `AIRCRAFT APPROACHING <n>S` (seconds to arrival)
- `OSD_ADSB_CONE` — a two-row **collision cone**:

  The cone belongs to the **aircraft** (its approach cone); **both markers are you** inside it:

  ```
  -10-------0-------+10     scale spanning ±(cone/2)°
       ✜    ▲               ▲ = you now · ✜ = you predicted (at ToA)
  ```

  The **arrow ▲** is **where you are now** in the aircraft's cone (centre = you're dead ahead of its nose = collision course). Its **shape** is your direction drawn in the aircraft's own frame, which the bar draws with the aircraft's nose pointing **up** (its cone opens towards you from below): **▲ up** = same course as the aircraft, **▼ down** = flying straight at it, **◄ / ►** = crossing, on the same side as the position axis. If your own heading isn't trustworthy yet (no compass and GPS course not acquired) it becomes **`?`** (position still valid, only direction unknown).

  The **crosshair ✜** is **where you'll be, inside the aircraft's current cone, after the threat's time-to-arrival** (distance ÷ aircraft speed), moving only by your own velocity — same frame as the arrow, so hovering keeps it on the arrow and a sidestep shifts it in proportion to the range (e.g. 10 m/s for 60 s at 3 km ≈ 11°). The cone itself is rebuilt from the aircraft's latest position and heading on every update, so it follows the incoming aircraft. If projected to leave the cone it becomes an **outward arrow at that edge** (you're clearing). Watch the gap between arrow and crosshair: **closing toward centre = worsening, opening / leaving = resolving.** Uses GPS ground course (works without a compass); hidden when heading can't be trusted.

Place every element with its `osd_..._pos` CLI key, or visually with the [OSD layout tool](#tools). New OSD elements aren't known to the stock Configurator, so they must be positioned via CLI / the tool.

> ✅ **Validated 2026-10-01** with the ESP32 injector: the predicted-position crosshair behaves as described, and the **left/right sign of the scale is correct** — checked with traffic coming in from the west, using the setting sun as the reference. The direction arrow was mirrored on both axes in that same test and has been fixed since (it is drawn in the aircraft's frame, nose up, like the markers).

### Bench testing without a receiver

`mytaflight-tools/adsb-injector/` is an **ESP32 sketch** that emulates a TT-SC1: it raises a Wi-Fi access point (`10.0.0.1`) hosting a web page where you set lat/long/speed/heading/altitude/type/callsign for up to 5 aircraft, optionally make them **move**, and it streams valid MAVLink `ADSB_VEHICLE` frames out its UART into the FC — so you can exercise the whole OSD/alert chain on the bench.

---

## D — Vector "cruise" position hold (retired — now stock Betaflight ✅)

This fork used to add an iNAV-style mode where, inside POS HOLD, stick deflection commands a **wind-compensated velocity** instead of handing over to raw angle mode (field-validated on a DAKEFPV H743). **Betaflight 2026.6.1 implemented the same concept natively**: in position hold, sticks now command a target velocity (full stick = `ap_max_velocity`, with feedforward), the position fence follows the craft, and releasing the sticks brakes to a hold — no configuration needed. Our implementation was therefore retired during the rebase onto stable; the historical branch lives on as `backup-alpha/feature/poshold-vector`. The `pos_hold_navmode` CLI setting no longer exists.

---

## Upstream bug fixes

Two defects in stock Betaflight `2026.6.1` are fixed here. Unlike features A–C these are **not gated**: they change behaviour that upstream ships, in every build of this fork.

### GPS Rescue returned to the arming point instead of home

**Symptom (field-observed).** With `gps_set_home_point_once = ON`, land 200 m away from home, re-arm without a power cycle, then trigger RTH: the craft flies to the **re-arm point**, while the OSD home arrow and distance still point at the real home. The failsafe GPS-Rescue procedure is affected the same way.

**Cause.** The legacy rescue controller never reads `GPS_home_llh`. It reads the position estimator, whose XY origin is the point where horizontal fusion started — normally the arming point (`gps_rescue_multirotor.c`: *"relative to arming location, not absolute"*). `distanceToHomeCm` is the norm of that vector and the target step is its negation, so the rescue's "home" **is** the arming point. With the default `gps_set_home_point_once = OFF` home is re-set at every arm, the two coincide, and the bug stays hidden. Regression from upstream `GPS Rescue2026_6a (#15382)`.

**Fix** (`feature/rescue-home-fix`). In `sensorUpdate()`, subtract the origin→`GPS_home_llh` offset (`GPS_distance2d`, obtained through the same `positionEstimatorGetGpsOrigin()` API the flight-plan rescue builder uses) from the estimated position, so distance, bearing and target step all refer to the home the pilot sees. ~15 lines, +336 B of flash. Verified on the host: hovering over home, stock reports 250 m to home and would fly away; patched reports 0 m.

Upstream's own flight-plan rescue (`ENABLE_RESCUE_PLAN`) already builds its waypoints on `GPS_home_llh` and is not affected — but it needs `USE_FLIGHT_PLAN`, defined in `2026.6.1` **only for SITL targets**, so every real build runs the legacy path. Building with the Flight Plan option (+24 KB) makes this fix inert and also swaps in a completely different guidance and landing controller.

> ℹ️ **Adjacent gotcha, not a bug.** Pos hold and GPS Rescue need a valid heading, and a working compass is *not* accepted unless `trust_mag = ON` (default `OFF`). Otherwise heading must be re-learned from GPS course-over-ground with straight flight above 1 m/s **after every arm**. The flight-plan rescue does the same thing, holding and pitching forward, and aborts the mission if heading never becomes valid. Calibrate and verify the compass, then `set trust_mag = ON`.

### OSD elements were forced inside the canvas

**Symptom.** Elements placed outside the 53×20 HD canvas — a WTFOS 60×22 layout, say — are pushed to the canvas edge, and the change becomes permanent at the next save.

**Cause.** `osdInit()` rewrote every off-canvas position to `cols-1`/`rows-1` and stored it back into the element config; any later save (CLI `save`, the stats save at disarm, the Configurator) persisted it. Upstream code, present in `2026.6.1`.

**Fix** (`feature/osd-canvas`). Saved positions are never rewritten. Off-canvas elements simply aren't shown: the MAX7456 and framebuffer drivers bound-check their writes, and MSP forwards the coordinates to the goggles. The same branch restores the `osd_canvas_width` / `osd_canvas_height` CLI settings (under `OSD_CANVAS_TOOL`, enabled by default in the build tool), which are what let you declare a 60×22 WTFOS canvas in the first place.

> ⚠️ Positions already clamped by an earlier firmware cannot be recovered — paste back a CLI `diff` saved before the clamp happened.

---

## Tools

Browser-based helpers in `mytaflight-tools/`:

- **`osd-layout.html`** — open in a browser: paste a CLI `diff`/`dump`, drag the OSD elements (including the new ones) onto a video-system-aware grid (Analog, HDZero, DJI O3/O4, WTFOS, Walksnail, custom), then copy the `set osd_*_pos` lines back. WYSIWYG, with background overlay and 3 OSD profiles.
- **`build-tool.py`** — `python3 mytaflight-tools/build-tool.py` → <http://localhost:8792>. Pick a target and features from dropdowns/checkboxes (Full vs Slim cloud build), it runs `make`, reports flash usage, and serves the resulting `.hex`.
- **`adsb-injector/`** — the ESP32 ADS-B traffic simulator described above.

---

## Branches

| Branch | Contents |
|--------|----------|
| `master` | pristine upstream Betaflight base |
| `feature/battery-impedance` | Feature A |
| `feature/temp-sensors` | Feature B (+ tools, historically) |
| `feature/adsb` | Feature C |
| `feature/osd-canvas` | canvas CLI + no position clamping ([fix](#osd-elements-were-forced-inside-the-canvas)) |
| `feature/rescue-home-fix` | GPS Rescue returns to home ([fix](#gps-rescue-returned-to-the-arming-point-instead-of-home)) |
| **`integration`** | **all features + fixes + tools — flash this one** |

Feature branches are independent (each off `master`) for clean rebasing onto upstream; `integration` is where they come together.

> ⚠️ Adding OSD elements bumps the OSD parameter-group version, so flashing a build that changes the OSD element set **resets OSD element positions to defaults**. Save your CLI `diff` first and paste it back afterwards (or re-place with the OSD tool).

---

Below is the upstream Betaflight README.

---

![Betaflight](https://raw.githubusercontent.com/betaflight/.github/main/profile/images/bf_logo.svg#gh-light-mode-only)
![Betaflight](https://raw.githubusercontent.com/betaflight/.github/main/profile/images/bf_logo_dark.svg#gh-dark-mode-only)

[![Latest version](https://img.shields.io/github/v/release/betaflight/betaflight)](https://github.com/betaflight/betaflight/releases) [![Build](https://img.shields.io/github/actions/workflow/status/betaflight/betaflight/push.yml?branch=master)](https://github.com/betaflight/betaflight/actions/workflows/push.yml) [![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0) [![Join us on Discord!](https://img.shields.io/discord/868013470023548938)](https://discord.gg/n4E6ak4u3c)

Betaflight is flight controller software (firmware) used to fly multi-rotor craft and fixed wing craft. Betaflight focuses on flight performance, leading-edge feature additions, and wide target support.

## Release Schedule

| Date       | Release | Stage             | Status    |
| ---------- | ------- | ----------------- | --------- |
| 26-12-2025 | 2025.12 | Release           | Completed |
| 01-04-2026 | 2026.6  | Beta              |           |
| 01-05-2026 | 2026.6  | Release Candidate |           |
| 01-06-2026 | 2026.6  | Release           |           |
| 01-10-2026 | 2026.12 | Beta              |           |
| 01-11-2026 | 2026.12 | Release Candidate |           |
| 01-12-2026 | 2026.12 | Release           |           |

## News

### 📣 Announcement: New Versioning Scheme & Release Cadence 📣

To create a more predictable release schedule, we're moving to a new versioning system and development cycle, starting with the next release.

**New Format**: `YYYY.M.PATCH` (e.g., `2025.12.1`)

**Release Cadence**: Two major releases per year.

**Target Months**: June and December.

This means the successor to our current `4.x` series will be Betaflight `2025.12.x`, followed by Betaflight `2026.6.x`. We will also align the Betaflight App and Firmware to the same `YYYY.M.PATCH` releases (and cadence).

**Our New Release Cycle**

To support this schedule, our development phases will be structured as follows:

**Alpha**: For new feature development. Alpha builds for the next version will be available shortly after a stable release is published.

**Beta**: A one-month feature freeze for bug fixes only, and existing pull requests currently being reviewed, starting approximately two months before a release.

**Release Candidate (RC)**: A one-month period for final stabilization and testing before the official release.

### Requirements for the submission of new and updated target configuration

The requirements for pull requests adding new targets or modifying existing targets are available on the [betaflight.com website](https://www.betaflight.com/docs/development/manufacturer/requirements-for-submission-of-targets).

## Features

Betaflight has the following features:

- Multi-color RGB LED strip support (each LED can be a different color using variable length WS2811 Addressable RGB strips - use for Orientation Indicators, Low Battery Warning, Flight Mode Status, Initialization Troubleshooting, etc)
- DShot (150, 300 and 600), Multishot, Oneshot (125 and 42) and Proshot1000 motor protocol support
- Blackbox flight recorder logging (to onboard flash or external microSD card where equipped)
- Support for targets that use the STM32 F4, G4, F7 and H7 processors
- PWM, PPM, SPI, and Serial (SBus, SumH, SumD, Spektrum 1024/2048, XBus, etc) RX connection with failsafe detection
- Multiple telemetry protocols (CRSF, FrSky, HoTT smart-port, MSP, etc)
- RSSI via ADC - Uses ADC to read PWM RSSI signals, tested with FrSky D4R-II, X8R, X4R-SB, & XSR
- OSD support & configuration without needing third-party OSD software/firmware/comm devices
- OLED Displays - Display information on: Battery voltage/current/mAh, profile, rate profile, mode, version, sensors, etc
- In-flight manual PID tuning and rate adjustment
- PID and filter tuning using sliders
- Rate profiles and in-flight selection of them
- Configurable serial ports for Serial RX, Telemetry, ESC telemetry, MSP, GPS, OSD, Sonar, etc - Use most devices on any port, softserial included
- VTX support for Unify Pro and IRC Tramp
- and MUCH, MUCH more.

## Installation & Documentation

See: https://betaflight.com/docs/wiki

## Support and Developers Channel

There's a dedicated [Discord server](https://discord.gg/n4E6ak4u3c) for help, support and general community.

## Betaflight Application

To configure Betaflight you should use the [Betaflight App](https://app.betaflight.com). It is a progressive web app, so should always be the latest version.

## Contributing

Contributions are welcome and encouraged. You can contribute in many ways:

- implement a new feature in the firmware or in the app (see [below](#developers));
- documentation updates and corrections;
- How-To guides - received help? Help others!
- bug reporting & fixes;
- new feature ideas & suggestions;
- provide a new translation for the app, or help us maintain the existing ones (see [below](#translators)).

The best place to start is the Betaflight Discord (registration [here](https://discord.gg/n4E6ak4u3c)). Next place is the github issue tracker:

https://github.com/betaflight/betaflight/issues
https://github.com/betaflight/betaflight-configurator/issues

Before creating new issues please check to see if there is an existing one, search first otherwise you waste people's time when they could be coding instead!

If you want to contribute to our efforts financially, please consider making a donation to us through [PayPal](https://paypal.me/betaflight).

If you want to contribute financially on an ongoing basis, you should consider becoming a patron for us on [Patreon](https://www.patreon.com/betaflight).

## Developers

Contribution of bugfixes and new features is encouraged. Please be aware that we have a thorough review process for pull requests, and be prepared to explain what you want to achieve with your pull request.
Before starting to write code, please read our [development guidelines](https://www.betaflight.com/docs/development) and [coding style definition](https://www.betaflight.com/docs/development/CodingStyle).

GitHub actions are used to run automatic builds.

### Building with Docker/Devcontainers

A preconfigured [devcontainer](.devcontainer/README.md) is included for a consistent build environment across all platforms. This is the recommended approach for Windows developers:

```bash
# With VS Code: Install "Dev Containers" extension, open folder, and select "Reopen in Container"

# Or command-line only:
docker build -t betaflight-dev -f .devcontainer/containerfile .devcontainer/
docker run --rm -v "${PWD}:/workspace" -w /workspace betaflight-dev make TARGET=SPEEDYBEEF405WING
```

See the [devcontainer documentation](.devcontainer/README.md) for detailed setup instructions including hardware flashing.

## Translators

We want to make Betaflight accessible for pilots who are not fluent in English, and for this reason we are currently maintaining translations into 21 languages for Betaflight Configurator: Català, Dansk, Deutsch, Español, Euskera, Français, Galego, Hrvatski, Bahasa Indonesia, Italiano, 日本語, 한국어, Latviešu, Português, Português Brasileiro, polski, Русский язык, Svenska, 简体中文, 繁體中文.
We have got a team of volunteer translators who do this work, but additional translators are always welcome to share the workload, and we are keen to add additional languages. If you would like to help us with translations, you have got the following options:

- if you help by suggesting some updates or improvements to translations in a language you are familiar with, head to [crowdin](https://crowdin.com/project/betaflight-configurator) and add your suggested translations there;
- if you would like to start working on the translation for a new language, or take on responsibility for proof-reading the translation for a language you are very familiar with, please head to the Betaflight Discord chat (registration [here](https://discord.gg/n4E6ak4u3c)), and join the ['translation'](https://discord.com/channels/868013470023548938/1057773726915100702) channel - the people in there can help you to get a new language added, or set you up as a proof reader.

## Hardware Issues

Betaflight does not manufacture or distribute their own hardware. While we are collaborating with and supported by a number of manufacturers, we do not do any kind of hardware support.

If you encounter any hardware issues with your flight controller or another component, please contact the manufacturer or supplier of your hardware, or check [Discord](https://discord.gg/n4E6ak4u3c) to see if others with the same problem have found a solution.

## Betaflight Releases

You can find our release [here](https://github.com/betaflight/betaflight/releases) on Github and we also have more detailed [release notes](https://www.betaflight.com/docs/category/release-notes) at [betaflight.com](https://www.betaflight.com).

## Open Source / Contributors

Betaflight is software that is **open source** and is available free of charge without warranty to all users.

For a complete list of contributors (past and present) see [Github](https://github.com/betaflight/betaflight/graphs/contributors).
