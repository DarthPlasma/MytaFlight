// Validate the injector's motion + geometry + threat-predictor logic (copied verbatim from
// adsb-injector.ino) on the host: fly an aircraft toward home and confirm distance/ToA fall and
// the threat flag triggers, then flip a receding aircraft and confirm it never triggers.
#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define NUM_AC 5
static const double M_PER_DEG_LAT = 111320.0;
struct Aircraft { bool enabled; double lat, lon, altAmslM, initLat, initLon, initAlt;
                  int32_t speedKmh, headingDeg, vsMps; uint8_t emitter; char callsign[9]; };
static struct Aircraft ac[NUM_AC];
static double homeLat = 45.0, homeLon = 9.0;
static int coneDeg = 20, toaMaxS = 60;

static void integrateMotion(double dtSec) {
  for (int i = 0; i < NUM_AC; i++) { if (!ac[i].enabled) continue;
    const double mps = ac[i].speedKmh / 3.6, dist = mps * dtSec, rad = ac[i].headingDeg * M_PI / 180.0;
    ac[i].lat += (dist * cos(rad)) / M_PER_DEG_LAT;
    ac[i].lon += (dist * sin(rad)) / (M_PER_DEG_LAT * cos(ac[i].lat * M_PI / 180.0));
    ac[i].altAmslM += ac[i].vsMps * dtSec; } }
static void distBearing(int i, double *distM, double *brgDeg) {
  const double dN = (ac[i].lat - homeLat) * M_PER_DEG_LAT;
  const double dE = (ac[i].lon - homeLon) * M_PER_DEG_LAT * cos(homeLat * M_PI / 180.0);
  *distM = sqrt(dN * dN + dE * dE); double b = atan2(dE, dN) * 180.0 / M_PI; if (b < 0) b += 360.0; *brgDeg = b; }
static bool isThreatCandidate(int i, double distM, double brgDeg, double *toaOut) {
  const double mps = ac[i].speedKmh / 3.6, toa = (mps > 0.1) ? (distM / mps) : 1e9; *toaOut = toa;
  if (toa > toaMaxS) return false;
  double hErr = ac[i].headingDeg - (brgDeg + 180.0);
  hErr = fmod(fmod(hErr, 360.0) + 360.0, 360.0); if (hErr > 180.0) hErr -= 360.0;
  return fabs(hErr) <= coneDeg / 2.0; }

static void placeRelative(int i, double distM, double brgDeg, int hdg) {
  double rad = brgDeg * M_PI / 180.0;
  ac[i].lat = homeLat + (distM * cos(rad)) / M_PER_DEG_LAT;
  ac[i].lon = homeLon + (distM * sin(rad)) / (M_PER_DEG_LAT * cos(homeLat * M_PI / 180.0));
  ac[i].headingDeg = hdg; ac[i].enabled = true; }

int main(void) {
  // #0 approaching from NE (bearing 45), heading 225 (back at home), 300 km/h.
  ac[0] = (struct Aircraft){0}; placeRelative(0, 5000, 45, 225); ac[0].speedKmh = 300;
  // #1 receding to the N (bearing 0), heading 0 (flying away), 300 km/h -> should never be a threat.
  ac[1] = (struct Aircraft){0}; placeRelative(1, 3000, 0, 0); ac[1].speedKmh = 300;

  printf("t     #0 dist  toa  thr   #1 dist  toa  thr\n");
  bool zeroBecameThreat = false, oneEverThreat = false;
  double prevDist0 = 1e9;
  bool distMonotonicDown = true;
  for (int t = 0; t <= 60; t += 10) {
    double d0, b0, toa0, d1, b1, toa1;
    distBearing(0, &d0, &b0); bool th0 = isThreatCandidate(0, d0, b0, &toa0);
    distBearing(1, &d1, &b1); bool th1 = isThreatCandidate(1, d1, b1, &toa1);
    printf("%3ds  %7.0f %4.0f  %s   %7.0f %4.0f  %s\n", t, d0, toa0, th0?"YES":"no ", d1, toa1, th1?"YES":"no ");
    if (th0) zeroBecameThreat = true;
    if (th1) oneEverThreat = true;
    if (t > 0 && d0 > prevDist0 + 1) distMonotonicDown = false;
    prevDist0 = d0;
    integrateMotion(10.0);
  }

  int pass = 1;
  printf("\n");
  printf("  [%s] approaching #0 distance decreases over time\n", distMonotonicDown?"OK":"!!"); pass &= distMonotonicDown;
  printf("  [%s] approaching #0 eventually flagged as threat (cone+ToA)\n", zeroBecameThreat?"OK":"!!"); pass &= zeroBecameThreat;
  printf("  [%s] receding #1 never flagged as threat\n", !oneEverThreat?"OK":"!!"); pass &= !oneEverThreat;
  printf("\n%s\n", pass?"PASS":"FAIL");
  return pass?0:1;
}
