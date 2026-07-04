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
 */

#pragma once

#include "common/time.h"

#include "pg/pg.h"

// Global/hardware settings for the external I2C temperature sensor (e.g. LM75).
typedef struct temperatureSensorConfig_s {
    uint8_t i2c_device;    // I2C bus the sensor is wired to (I2CDEV_x)
    uint8_t i2c_address;   // 7-bit I2C address (LM75 default 0x48)
    uint8_t alarmMinC;     // low alarm threshold, whole degrees Celsius (Tlow)
    uint8_t alarmMaxC;     // high alarm threshold, whole degrees Celsius (Thigh)
} temperatureSensorConfig_t;

PG_DECLARE(temperatureSensorConfig_t, temperatureSensorConfig);

void temperatureSensorInit(void);
void temperatureSensorUpdate(timeUs_t currentTimeUs);
bool temperatureSensorIsPresent(void);
int16_t getTemperatureSensorDeciDegrees(void);  // temperature in 0.1 C units (valid only when present)
