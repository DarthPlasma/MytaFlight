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

#include "io/adsb.h"
#include "io/gps.h"

// MAVLink ADSB_FLAGS bits we require (mirror the MAVLink common dialect values).
#define ADSB_FLAGS_VALID_COORDS   1
#define ADSB_FLAGS_VALID_ALTITUDE 2
// Drop vehicles farther than this (64 km), beyond useful traffic-awareness range.
#define ADSB_LIMIT_CM (64L * 1000 * 100)

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
    // ADSB altitude is mm ASL; our GPS altitude is cm ASL. Convert both to cm.
    vehicle->calculatedVehicleValues.verticalDistance = (vehicle->vehicleValues.alt / 10) - gpsSol.llh.altCm;

    if (dist > ADSB_LIMIT_CM) {
        vehicle->ttl = 0;
        vehicle->calculatedVehicleValues.valid = false;
    } else {
        vehicle->calculatedVehicleValues.valid = true;
    }
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

#endif // USE_ADSB
