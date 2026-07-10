/*
 * This file is part of Betaflight.
 *
 * Betaflight is free software. You can redistribute this software
 * and/or modify this software under the terms of the GNU General
 * Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later
 * version.
 *
 * Betaflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 *
 * ADS-B traffic awareness. Adapted from iNAV (src/main/io/adsb.c), GPLv3.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"

#ifdef USE_ADSB

#include "common/maths.h"
#include "common/utils.h"

#include "fc/runtime_config.h"

#include "flight/position.h"

#include "pg/pg.h"
#include "pg/pg_ids.h"

#include "io/adsb.h"
#include "io/gps.h"

// MAVLink ADSB_FLAGS bits (mirror the MAVLink common dialect values).
#define ADSB_FLAGS_VALID_COORDS   1
#define ADSB_FLAGS_VALID_ALTITUDE 2
#define ADSB_FLAGS_VALID_CALLSIGN 16

PG_REGISTER_WITH_RESET_TEMPLATE(adsbConfig_t, adsbConfig, PG_ADSB_CONFIG, 0);

PG_RESET_TEMPLATE(adsbConfig_t, adsbConfig,
    .maxDistHorizM = 50000, // 50 km
    .maxDistVertM  = 2000,  // 2 km above us
    .detectionCone = 2000,  // +/-10 degrees
    .toaSeconds    = 60,
);

// Our own altitude (ASL, cm) from the fused estimate: home altitude (ASL) plus the
// baro/GPS/accel altitude estimate relative to home. Falls back to raw GPS if no home.
static int32_t ourAltitudeAslCm(void)
{
    if (STATE(GPS_FIX_HOME)) {
        return GPS_home_llh.altCm + getEstimatedAltitudeCm();
    }
    return gpsSol.llh.altCm;
}

// Slots for tracked aircraft. Slot 0..ADSB_MAX_VEHICLES-1; a ttl of 0 means the slot is free.
static adsbVehicle_t vehiclesList[ADSB_MAX_VEHICLES];
static adsbVehicleStatus_t adsbVehicleStatus = { .vehiclesMessagesTotal = 0, .heartbeatMessagesTotal = 0 };
// Scratch record the MAVLink handler fills before handing it to adsbNewVehicle().
static adsbVehicleValues_t vehicleForFill;

// Short class labels indexed by MAVLink ADSB_EMITTER_TYPE (same taxonomy iNAV uses).
static const char * const adsbEmitterTypeStrings[] = {
    "UNKN",   // 0  NO_INFO
    "LIGHT",  // 1  LIGHT
    "SMALL",  // 2  SMALL
    "LARGE",  // 3  LARGE
    "VORTEX", // 4  HIGH_VORTEX_LARGE
    "HEAVY",  // 5  HEAVY
    "HIMANV", // 6  HIGHLY_MANUV
    "ROTOR",  // 7  ROTOCRAFT
    "UNKN",   // 8  UNASSIGNED
    "GLIDER", // 9  GLIDER
    "LTAIR",  // 10 LIGHTER_AIR
    "PARA",   // 11 PARACHUTE
    "ULTLT",  // 12 ULTRA_LIGHT
    "UNKN",   // 13 UNASSIGNED2
    "UAV",    // 14 UAV
    "SPACE",  // 15 SPACE
    "UNKN",   // 16 UNASSIGNED3
    "EMGVEH", // 17 EMERGENCY_SURFACE
    "SRVVEH", // 18 SERVICE_SURFACE
    "OBSTAC", // 19 POINT_OBSTACLE
};

const char *adsbEmitterTypeString(uint8_t emitterType)
{
    if (emitterType < ARRAYLEN(adsbEmitterTypeStrings)) {
        return adsbEmitterTypeStrings[emitterType];
    }
    return "UNKN";
}

static adsbVehicle_t *findVehicleByIcao(uint32_t icao)
{
    for (int i = 0; i < ADSB_MAX_VEHICLES; i++) {
        if (vehiclesList[i].ttl > 0 && vehiclesList[i].vehicleValues.icao == icao) {
            return &vehiclesList[i];
        }
    }
    return NULL;
}

adsbVehicleValues_t *getVehicleForFill(void)
{
    return &vehicleForFill;
}

adsbVehicleStatus_t *getAdsbStatus(void)
{
    return &adsbVehicleStatus;
}

adsbVehicle_t *findVehicle(uint8_t index)
{
    return (index < ADSB_MAX_VEHICLES) ? &vehiclesList[index] : NULL;
}

uint8_t getActiveVehiclesCount(void)
{
    uint8_t count = 0;
    for (int i = 0; i < ADSB_MAX_VEHICLES; i++) {
        if (vehiclesList[i].ttl > 0) {
            count++;
        }
    }
    return count;
}

bool isEnvironmentOkForCalculatingADSBDistanceBearing(void)
{
    return STATE(GPS_FIX) && (gpsSol.numSat > 4);
}

void recalculateVehicle(adsbVehicle_t *vehicle)
{
    if (!isEnvironmentOkForCalculatingADSBDistanceBearing()) {
        vehicle->calculatedVehicleValues.valid = false;
        return;
    }

    uint32_t dist;
    int32_t bearing;
    GPS_distance_cm_bearing(&gpsSol.llh, &vehicle->vehicleValues.gps, false, &dist, &bearing);

    vehicle->calculatedVehicleValues.dist = dist;
    vehicle->calculatedVehicleValues.dir = bearing; // centidegrees, clockwise from North
    // ADSB altitude is mm ASL; our fused altitude is cm ASL. Positive = aircraft above us.
    vehicle->calculatedVehicleValues.verticalDistance = (vehicle->vehicleValues.alt / 10) - ourAltitudeAslCm();

    // "valid" just means distance/bearing were computed (we have a GPS fix). Do NOT evict distant
    // traffic here: getActiveVehiclesCount() ("A<x>/...") must reflect everything received over
    // MAVLink, however far. Display/threat filtering by distance & height is done separately by
    // vehicleWithinDisplayLimits(); the 5 slots are recycled via TTL + adsbNewVehicle()'s
    // farthest-first eviction when a new aircraft needs a slot.
    vehicle->calculatedVehicleValues.valid = true;
}

void adsbNewVehicle(adsbVehicleValues_t *vehicleValues)
{
    adsbVehicleStatus.vehiclesMessagesTotal++;

    if (vehicleValues->icao == 0) {
        return;
    }

    // Require both a valid position and altitude, otherwise we cannot place the aircraft.
    const uint16_t requiredFlags = ADSB_FLAGS_VALID_ALTITUDE | ADSB_FLAGS_VALID_COORDS;
    if ((vehicleValues->flags & requiredFlags) != requiredFlags) {
        return;
    }

    adsbVehicle_t *slot = findVehicleByIcao(vehicleValues->icao);

    // Stale report: drop the aircraft if we were tracking it.
    if (vehicleValues->tslc > ADSB_MAX_SECONDS_KEEP_INACTIVE_PLANE_IN_LIST) {
        if (slot) {
            slot->ttl = 0;
        }
        return;
    }

    // Not already tracked: pick a slot — free first, then an un-calculated one, then evict the farthest.
    if (!slot) {
        for (int i = 0; i < ADSB_MAX_VEHICLES && !slot; i++) {
            if (vehiclesList[i].ttl == 0) {
                slot = &vehiclesList[i];
            }
        }
    }
    if (!slot) {
        for (int i = 0; i < ADSB_MAX_VEHICLES && !slot; i++) {
            if (!vehiclesList[i].calculatedVehicleValues.valid) {
                slot = &vehiclesList[i];
            }
        }
    }
    if (!slot && isEnvironmentOkForCalculatingADSBDistanceBearing()) {
        uint32_t farthest = 0;
        for (int i = 0; i < ADSB_MAX_VEHICLES; i++) {
            if (vehiclesList[i].calculatedVehicleValues.dist >= farthest) {
                farthest = vehiclesList[i].calculatedVehicleValues.dist;
                slot = &vehiclesList[i];
            }
        }
    }

    if (slot) {
        slot->vehicleValues = *vehicleValues;
        if (!(vehicleValues->flags & ADSB_FLAGS_VALID_CALLSIGN)) {
            memset(slot->vehicleValues.callsign, 0, sizeof(slot->vehicleValues.callsign)); // avoid showing junk
        }
        slot->ttl = ADSB_MAX_SECONDS_KEEP_INACTIVE_PLANE_IN_LIST;
        recalculateVehicle(slot);
    }
}

adsbVehicle_t *findVehicleClosestLimit(int32_t maxVerticalDistance)
{
    adsbVehicle_t *closest = NULL;
    for (int i = 0; i < ADSB_MAX_VEHICLES; i++) {
        adsbVehicle_t *vehicle = &vehiclesList[i];
        if (vehicle->ttl == 0 || !vehicle->calculatedVehicleValues.valid) {
            continue;
        }
        if (maxVerticalDistance > 0 && ABS(vehicle->calculatedVehicleValues.verticalDistance) > maxVerticalDistance) {
            continue;
        }
        if (!closest || vehicle->calculatedVehicleValues.dist < closest->calculatedVehicleValues.dist) {
            closest = vehicle;
        }
    }
    return closest;
}

void adsbTtlClean(timeUs_t currentTimeUs)
{
    static timeUs_t lastCleanUs = 0;
    if (cmpTimeUs(currentTimeUs, lastCleanUs) < 1000000) { // once per second
        return;
    }
    lastCleanUs = currentTimeUs;

    for (int i = 0; i < ADSB_MAX_VEHICLES; i++) {
        if (vehiclesList[i].ttl > 0) {
            vehiclesList[i].ttl--;
            recalculateVehicle(&vehiclesList[i]);
        }
    }
}

// Passes the configured display limits: within the horizontal distance, and not more than
// maxDistVertM ABOVE us (traffic at or below our altitude is always shown).
static bool vehicleWithinDisplayLimits(const adsbVehicle_t *vehicle)
{
    if (vehicle->calculatedVehicleValues.dist > (uint32_t)adsbConfig()->maxDistHorizM * 100) {
        return false;
    }
    if (vehicle->calculatedVehicleValues.verticalDistance > (int32_t)adsbConfig()->maxDistVertM * 100) {
        return false;
    }
    return true;
}

adsbVehicle_t *findVehicleClosestForDisplay(void)
{
    adsbVehicle_t *closest = NULL;
    for (int i = 0; i < ADSB_MAX_VEHICLES; i++) {
        adsbVehicle_t *vehicle = &vehiclesList[i];
        if (vehicle->ttl == 0 || !vehicle->calculatedVehicleValues.valid || !vehicleWithinDisplayLimits(vehicle)) {
            continue;
        }
        if (!closest || vehicle->calculatedVehicleValues.dist < closest->calculatedVehicleValues.dist) {
            closest = vehicle;
        }
    }
    return closest;
}

uint8_t getVehiclesInDisplayRangeCount(void)
{
    uint8_t count = 0;
    for (int i = 0; i < ADSB_MAX_VEHICLES; i++) {
        adsbVehicle_t *vehicle = &vehiclesList[i];
        if (vehicle->ttl > 0 && vehicle->calculatedVehicleValues.valid && vehicleWithinDisplayLimits(vehicle)) {
            count++;
        }
    }
    return count;
}

adsbVehicle_t *findVehicleThreat(uint32_t *toaSecondsOut)
{
    adsbVehicle_t *threat = NULL;
    uint32_t threatToa = 0;
    for (int i = 0; i < ADSB_MAX_VEHICLES; i++) {
        adsbVehicle_t *vehicle = &vehiclesList[i];
        if (vehicle->ttl == 0 || !vehicle->calculatedVehicleValues.valid) {
            continue;
        }
        // c) within the same configured display range as findVehicleClosestForDisplay() /
        //    getVehiclesInDisplayRangeCount(), so the threat, the "in range" count, and the
        //    closest-for-display fallback all agree on what's in range.
        if (!vehicleWithinDisplayLimits(vehicle)) {
            continue;
        }
        // b) time-to-arrival = distance / ground speed, must be within toaSeconds
        if (vehicle->vehicleValues.horVelocity == 0) {
            continue;
        }
        const uint32_t timeToArrival = vehicle->calculatedVehicleValues.dist / vehicle->vehicleValues.horVelocity;
        if (timeToArrival > adsbConfig()->toaSeconds) {
            continue;
        }
        // a) the aircraft is heading toward us: its course is within +/-(cone/2) of the reciprocal
        //    of the bearing from us to it (dir + 180 deg).
        int32_t headingError = (int32_t)vehicle->vehicleValues.heading - (vehicle->calculatedVehicleValues.dir + 18000);
        headingError = ((headingError % 36000) + 36000) % 36000; // wrap to [0, 36000)
        if (headingError > 18000) {
            headingError -= 36000;                                // wrap to [-18000, 18000)
        }
        if (ABS(headingError) > adsbConfig()->detectionCone / 2) {
            continue;
        }
        // Among all qualifying vehicles, keep the one with the lowest time-to-arrival.
        if (!threat || timeToArrival < threatToa) {
            threat = vehicle;
            threatToa = timeToArrival;
        }
    }
    if (toaSecondsOut) {
        *toaSecondsOut = threatToa;
    }
    return threat;
}

#endif // USE_ADSB
