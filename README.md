# MyTAflight

MyTAflight is a **personal fork of [Betaflight](https://github.com/betaflight/betaflight)** that adds four traffic-safety / telemetry features (ported or adapted from iNAV) plus browser-based tooling. Every addition is **feature-gated and additive**: build with the custom flags off and you get stock Betaflight behaviour.

- **Base:** Betaflight `2026.6.0-alpha` · **Licence:** GPLv3
- **Flash this branch:** `integration` merges all four features + the tools.
- **Developer deep-dive:** see **[MYTAFLIGHT.md](MYTAFLIGHT.md)** for file-by-file changes, design notes and gotchas.

## What's added

|   | Feature | Build flag | Default | Requires |
|---|---------|-----------|---------|----------|
| **A** | Battery internal impedance + sag-compensated voltage | `USE_BATTERY_IMPEDANCE` | **on** | current meter |
| **B** | I²C temperature sensor (LM75) on the OSD | `USE_TEMPERATURE_SENSOR` | opt-in | LM75 sensor |
| **C** | ADS-B traffic awareness on the OSD | `USE_ADSB` | opt-in | GPS + MAVLink ADS-B RX |
| **D** | Vector "cruise" position hold | *(runtime)* `pos_hold_navmode` | classic `ANGLE` | GPS |

Build a firmware with the opt-in features:

```bash
git checkout integration
make KAKUTEH7 OPTIONS="USE_TEMPERATURE_SENSOR USE_ADSB"                   # H7 boards
make TMOTORVELOXF7V2 OPTIONS="USE_TEMPERATURE_SENSOR USE_ADSB USE_GPS"    # add USE_GPS where the config lacks it
```

Impedance (A) and cruise pos-hold (D) are always compiled in and controlled at runtime. Prefer a UI? Use the point-and-click [build tool](#tools).

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

  ```
  -10-------0-------+10     scale spanning ±(cone/2)°
       ✜    ▲               ✜ = our predicted position · ▲ = threat + relative-motion arrow
  ```

  The **threat arrow's column** is where the threat sits in the cone (centre = dead-on collision course); its **direction** is the angle between your motion vector and the aircraft's: **▲ up** = head-on (opposed vectors), **▼ down** = same course, **◄ / ►** = crossing traffic. If your own heading isn't trustworthy yet (no compass and GPS course not acquired) the arrow becomes **`?`** — position still valid (GPS-derived), only direction unknown.

  The **crosshair ✜** marks **where you'll be at the threat's time-to-arrival, relative to where the aircraft will be, if both hold course** — the classic constant-bearing/decreasing-range collision cue. Centre = you'll occupy its position (**collision**); off to a side = you'll pass that side; if you're projected to leave the cone it becomes an **outward arrow at that edge** (you're clearing). Watch the gap between the two markers: closing = the encounter is worsening, opening = resolving. Uses GPS ground course (works without a compass); hidden when heading can't be trusted.

Place every element with its `osd_..._pos` CLI key, or visually with the [OSD layout tool](#tools). New OSD elements aren't known to the stock Configurator, so they must be positioned via CLI / the tool.

### Bench testing without a receiver

`mytaflight-tools/adsb-injector/` is an **ESP32 sketch** that emulates a TT-SC1: it raises a Wi-Fi access point (`10.0.0.1`) hosting a web page where you set lat/long/speed/heading/altitude/type/callsign for up to 5 aircraft, optionally make them **move**, and it streams valid MAVLink `ADSB_VEHICLE` frames out its UART into the FC — so you can exercise the whole OSD/alert chain on the bench.

---

## D — Vector "cruise" position hold

Stock Betaflight position hold hands control back to **raw angle mode** on stick input, which drifts in wind and gives a false sense of control. This adds an iNAV-style option where, inside POS HOLD, stick deflection commands a **wind-compensated velocity** instead of an angle; releasing the sticks brakes smoothly to a hold.

- **Runtime setting:** `pos_hold_navmode` = `ANGLE` (default, unchanged classic behaviour) or `CRUISE`. Yaw stays pilot-controlled; cruise velocity magnitude uses `ap_max_velocity` (shared with waypoint nav).
- **Status:** merged and **field-validated** on a DAKEFPV H743 (correct stick signs, brake-to-hold, no crosswind drift). Only PID tuning (notably altitude) remains. The default stays `ANGLE`, so flashing changes nothing until you select `CRUISE`.

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
| `feature/poshold-vector` | Feature D |
| **`integration`** | **all four features + tools — flash this one** |

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
