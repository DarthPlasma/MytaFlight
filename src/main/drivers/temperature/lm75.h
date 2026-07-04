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

#include <stdbool.h>
#include <stdint.h>

#include "drivers/bus_i2c.h"

// Read the LM75 temperature register into deciDegrees (0.1 C). Returns false on I2C failure.
// deciDegrees may be NULL (used for presence probing).
bool lm75Read(i2cDevice_e device, uint8_t address, int16_t *deciDegrees);

// Probe for an LM75 on the given bus/address (a few retries). Returns true if it responds.
bool lm75Detect(i2cDevice_e device, uint8_t address);
