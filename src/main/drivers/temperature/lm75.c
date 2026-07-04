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
 * LM75 I2C temperature sensor driver. Conversion adapted from iNAV
 * (src/main/drivers/temperature/lm75.c), GPLv3.
 */

#include <stdbool.h>
#include <stdint.h>

#include "platform.h"

#ifdef USE_TEMPERATURE_SENSOR

#include "drivers/bus_i2c.h"
#include "drivers/time.h"

#include "drivers/temperature/lm75.h"

#define LM75_REG_TEMP          0x00 // temperature register (read-only)
#define LM75_DETECTION_RETRIES 5
#define LM75_DETECTION_DELAY_MS 10

bool lm75Read(i2cDevice_e device, uint8_t address, int16_t *deciDegrees)
{
    uint8_t buf[2];

    if (!i2cRead(device, address, LM75_REG_TEMP, sizeof(buf), buf)) {
        return false;
    }

    if (deciDegrees) {
        // byte0 = whole degrees (signed two's complement), byte1 MSB = 0.5 C
        *deciDegrees = (int16_t)((int8_t)buf[0]) * 10 + (buf[1] >> 7) * 5;
    }

    return true;
}

bool lm75Detect(i2cDevice_e device, uint8_t address)
{
    for (int retry = 0; retry < LM75_DETECTION_RETRIES; retry++) {
        delay(LM75_DETECTION_DELAY_MS);
        if (lm75Read(device, address, NULL)) {
            return true;
        }
    }

    return false;
}

#endif // USE_TEMPERATURE_SENSOR
