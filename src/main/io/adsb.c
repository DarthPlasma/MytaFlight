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

#include <string.h>

#include "platform.h"

#ifdef USE_ADSB

#include "common/utils.h"

#include "io/adsb.h"

// Slots for tracked aircraft. Slot 0..ADSB_MAX_VEHICLES-1; a ttl of 0 means the slot is free.
static adsbVehicle_t vehiclesList[ADSB_MAX_VEHICLES];
static adsbVehicleStatus_t adsbVehicleStatus = { .vehiclesMessagesTotal = 0, .heartbeatMessagesTotal = 0 };
// Scratch record the MAVLink handler fills before handing it to adsbNewVehicle().
static adsbVehicleValues_t vehicleForFill;

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
    // TODO (step 2): count slots with ttl > 0.
    return 0;
}

bool isEnvironmentOkForCalculatingADSBDistanceBearing(void)
{
    // TODO (step 2): require a valid GPS fix so distance/bearing can be computed.
    return false;
}

void recalculateVehicle(adsbVehicle_t *vehicle)
{
    // TODO (step 2): distance/bearing/vertical delta from gpsSol.llh via GPS_distance_cm_bearing().
    UNUSED(vehicle);
}

void adsbNewVehicle(adsbVehicleValues_t *vehicleValues)
{
    // TODO (step 2): insert/update by ICAO, evict oldest if full, then recalculate.
    UNUSED(vehicleValues);
}

adsbVehicle_t *findVehicleClosestLimit(int32_t maxVerticalDistance)
{
    // TODO (step 2): nearest active vehicle within +/- maxVerticalDistance.
    UNUSED(maxVerticalDistance);
    return NULL;
}

void adsbTtlClean(timeUs_t currentTimeUs)
{
    // TODO (step 2): decrement ttl once per second, freeing expired slots.
    UNUSED(currentTimeUs);
}

#endif // USE_ADSB
