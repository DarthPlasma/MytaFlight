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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "common/time.h"

#include "io/gps.h"

#ifdef USE_ADSB

#if !defined(USE_GPS)
#error "USE_ADSB requires USE_GPS (own position is needed for distance/bearing)"
#endif

#define ADSB_CALL_SIGN_MAX_LENGTH 9
#define ADSB_MAX_VEHICLES 5
#define ADSB_MAX_SECONDS_KEEP_INACTIVE_PLANE_IN_LIST 10

// Raw values received in an ADSB_VEHICLE MAVLink frame.
typedef struct {
    uint32_t icao;                              // ICAO address (unique aircraft id)
    uint16_t horVelocity;                       // horizontal velocity, cm/s
    gpsLocation_t gps;                          // lat/lon in 1e7 deg, altitude in cm
    int32_t alt;                                // altitude (ASL), mm
    uint16_t heading;                           // course over ground, cdeg
    uint16_t flags;                             // valid-field bitmap
    uint8_t altitudeType;
    char callsign[ADSB_CALL_SIGN_MAX_LENGTH];   // 8 chars + null
    uint8_t emitterType;                        // aircraft category
    uint8_t tslc;                               // time since last communication, s
} adsbVehicleValues_t;

// Values derived from our own position relative to the vehicle.
typedef struct {
    bool valid;
    int32_t dir;                                // bearing to the vehicle, centidegrees
    uint32_t dist;                              // distance to the vehicle, cm
    int32_t verticalDistance;                   // vehicle altitude minus ours, cm
} adsbVehicleCalculatedValues_t;

typedef struct {
    adsbVehicleValues_t vehicleValues;
    adsbVehicleCalculatedValues_t calculatedVehicleValues;
    uint8_t ttl;                                // seconds remaining before the slot is freed
} adsbVehicle_t;

typedef struct {
    uint32_t vehiclesMessagesTotal;
    uint32_t heartbeatMessagesTotal;
} adsbVehicleStatus_t;

void adsbNewVehicle(adsbVehicleValues_t *vehicleValues);
adsbVehicle_t *findVehicleClosestLimit(int32_t maxVerticalDistance);
adsbVehicle_t *findVehicle(uint8_t index);
uint8_t getActiveVehiclesCount(void);
void adsbTtlClean(timeUs_t currentTimeUs);
adsbVehicleStatus_t *getAdsbStatus(void);
adsbVehicleValues_t *getVehicleForFill(void);
bool isEnvironmentOkForCalculatingADSBDistanceBearing(void);
void recalculateVehicle(adsbVehicle_t *vehicle);

#endif // USE_ADSB
