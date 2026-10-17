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

#include <stdbool.h>
#include <stdint.h>

#include "platform.h"

#ifdef USE_TEMPERATURE_SENSOR

#include "common/utils.h"

#include "drivers/bus_i2c.h"
#include "drivers/temperature/lm75.h"

#include "pg/pg.h"
#include "pg/pg_ids.h"

#include "sensors/temperature.h"

#define TEMPERATURE_SENSOR_DEFAULT_I2C_ADDRESS 0x48 // LM75 base address (A0..A2 tied low)

PG_REGISTER_WITH_RESET_TEMPLATE(temperatureSensorConfig_t, temperatureSensorConfig, PG_TEMPERATURE_SENSOR_CONFIG, 0);

PG_RESET_TEMPLATE(temperatureSensorConfig_t, temperatureSensorConfig,
    .i2c_device = I2CDEV_1,
    .i2c_address = TEMPERATURE_SENSOR_DEFAULT_I2C_ADDRESS,
    .alarmMinC = 0,
    .alarmMaxC = 60,
);

static bool sensorPresent = false;
static int16_t sensorDeciDegrees = 0; // 0.1 C

void temperatureSensorInit(void)
{
    sensorPresent = lm75Detect(temperatureSensorConfig()->i2c_device, temperatureSensorConfig()->i2c_address);
}

void temperatureSensorUpdate(timeUs_t currentTimeUs)
{
    UNUSED(currentTimeUs);

    int16_t deciDegrees;
    if (lm75Read(temperatureSensorConfig()->i2c_device, temperatureSensorConfig()->i2c_address, &deciDegrees)) {
        sensorDeciDegrees = deciDegrees;
        sensorPresent = true;
    } else {
        // No response this cycle: report absent so the OSD shows dashes rather than a stale value.
        sensorPresent = false;
    }
}

bool temperatureSensorIsPresent(void)
{
    return sensorPresent;
}

int16_t getTemperatureSensorDeciDegrees(void)
{
    return sensorDeciDegrees;
}

#endif // USE_TEMPERATURE_SENSOR
