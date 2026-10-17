# Upstream issue draft — GPS Rescue returns to the arming point, not to home

Target: <https://github.com/betaflight/betaflight/issues/new?template=firmware-bug-report.yml>
Paste each section below into the matching field of the "Firmware Bug Report" form.
Two fields can only be filled by the pilot: **Support ID** and **Flight controller** (see the end).

---

## Describe the bug

GPS Rescue flies back to the point where the craft was **armed**, not to the **home point**, whenever
`gps_set_home_point_once = ON`. The OSD home arrow and home distance keep pointing at the real home, so
the two disagree and nothing on screen reveals it. The failsafe GPS-Rescue procedure is affected the same way.

Root cause: the legacy rescue controller never reads `GPS_home_llh`. `sensorUpdate()` copies the position
estimator's XY estimate (`src/main/flight/gps_rescue_multirotor.c:197`), whose origin is `armLocationGps`,
re-captured on every arm (`src/main/flight/position_estimator.c:282` and `:736`). The file's own comment says
so at `gps_rescue_multirotor.c:114`:

```c
vector2_t currentPositionV; // relative to arming location, not absolute
```

`rescueState.sensor.distanceToHomeCm` is the norm of that vector (`:201`) and the target step is
`-currentPositionV` (`:279`), so the rescue's "home" is by construction the arming point. Nothing reconciles it
with `GPS_home_llh`, which `GPS_reset_home_position()` deliberately keeps from the first fix when
`gps_set_home_point_once` is set (`src/main/io/gps.c:2934`).

With the default `gps_set_home_point_once = OFF` the home point is re-set at every arm, so origin == home and the
bug is invisible. It only shows with the setting ON plus a re-arm somewhere else without a power cycle — which is
exactly the situation that setting exists for.

Why it matters for safety: after landing away from home (a flip-over, a battery swap at the field edge, a hand
landing) the pilot re-arms on the spot and the return point silently changes, while the OSD still shows the
original home. A link loss then flies the craft to a place the pilot never chose.

Introduced by #15382 (GPS Rescue2026_6a). Present in `2026.6.1`, `2026.6.2` and current `master`
(checked at `6fd72d387`).

The flight-plan rescue is **not** affected: `buildRescuePlan()` places its waypoints on `GPS_home_llh`
(`src/main/flight/flight_plan_nav.c:922-976`) and converts them through `positionEstimatorGetGpsOrigin()`
(`:272`). But `ENABLE_RESCUE_PLAN` requires `USE_FLIGHT_PLAN`, which is defined only for SITL targets, so every
build for real hardware runs the legacy controller.

## To Reproduce

1. `set gps_set_home_point_once = ON`, configure GPS Rescue on a switch and/or as the failsafe procedure, `save`.
2. Power up, wait for a 3D fix, arm. The home point is set at position **A**.
3. Take off, fly ~200 m away and land at position **B**. **Do not disconnect the battery.**
4. Re-arm at **B**. The OSD home arrow and distance still correctly point at **A**.
5. Take off and activate GPS Rescue.

The craft returns to **B** and lands there, instead of returning to **A**.

A quicker check without a second flight: at step 5, hover over **A**. The rescue behaves as if it were
`distance(A,B)` away from home and flies away towards **B**, while the OSD home distance reads ~0.

## Expected behavior

GPS Rescue should return to `GPS_home_llh` — the home point the OSD displays, and the one
`gps_set_home_point_once` exists to preserve — not to the point where the craft was last armed.

## Other information

Suggested fix, re-referencing the estimated position to the home point inside `sensorUpdate()`, using the same
API the flight-plan rescue builder already uses:

```c
gpsLocation_t origin;
if (STATE(GPS_FIX_HOME) && positionEstimatorGetGpsOrigin(&origin)) {
    vector2_t homeFromOriginCm;
    GPS_distance2d(&origin, &GPS_home_llh, &homeFromOriginCm);
    rescueState.sensor.currentPositionV.v[EF_EAST]  -= homeFromOriginCm.v[EF_EAST];
    rescueState.sensor.currentPositionV.v[EF_NORTH] -= homeFromOriginCm.v[EF_NORTH];
}
```

Axis conventions line up: `EF_EAST == ENU_E` is asserted in `src/main/common/axis.h`, and `GPS_distance2d()`
writes `x` = East, `y` = North. With this in place `distanceToHomeCm`, the error angle and the target step all
refer to the real home. Verified on a host test replicating `GPS_distance2d`: with the re-arm point 200 m east and
150 m north of home, hovering over home the stock code reports 250 m to home (and flies away), the patched code
reports 0 m; at 500 m north of home, 403 m stock vs 500 m patched.

The fix is confined to the legacy path and becomes inert, without being dead code, once `ENABLE_RESCUE_PLAN`
is on. Happy to open a PR if the approach looks right.

**Disclosure:** the bug was found on a personal fork based on `2026.6.1`, whose only changes are OSD elements, a
battery-impedance estimator, an I2C temperature sensor and ADS-B traffic display — nothing in
`flight/gps_rescue_*` or `flight/position_estimator.c`. The code quoted above is identical to stock; I re-checked
it against the `2026.6.2` tag and `master`.

---

## Fields only the pilot can fill

- **Support ID** — required by the template. Configurator → CLI tab → `Submit Support Data`, then paste the ID.
  Note it will identify the build as a fork. If the maintainers push back, the clean answer is to flash **stock
  2026.6.2** on the test craft, repeat the 5 steps above and attach a blackbox log plus that Support ID.
- **Flight controller** — model and where it came from (e.g. DAKEFPV H743).
- **Other components / wiring** — GPS module and the UART it is on; receiver and VTX if you want to be thorough.
