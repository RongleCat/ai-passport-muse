/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * CellWise CW2017 fuel gauge at I2C 0x63.
 * The 80-byte profile and the raw-to-millivolt conversion are from
 * ai-passport-ref components/bsp/src/bsp_battery.c (MIT), the Ueteli 520 mAh
 * pack. Voltage is raw(14-bit) * 312.5 µV. SOC lives in registers 0x04-0x05;
 * a high byte above 100 means the gauge is not ready yet.
 *
 * The conversion helpers are host-compilable. The bus calls need ESP-IDF.
 */
#pragma once

#include <stdint.h>

#define PASSPORT_CW2017_ADDR 0x63
#define PASSPORT_CW2017_PROFILE_LEN 80

/* Ueteli 520 mAh profile. Exactly PASSPORT_CW2017_PROFILE_LEN bytes. */
extern const uint8_t passport_cw2017_profile[PASSPORT_CW2017_PROFILE_LEN];

/*
 * Pure: 14-bit cell reading to millivolts. Bits above bit 13 are masked off.
 * 312.5 µV/LSB = 5/16 mV, computed as (raw * 3125) / 10000.
 */
int passport_cw2017_raw_to_mv(uint16_t raw);

/*
 * Pure: SOC integer percent from the high byte (register 0x04).
 * Returns -1 when the byte is above 100 (gauge not ready, often 0xFF).
 */
int passport_cw2017_soc_percent(uint8_t soc_integer);

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#include "esp_err.h"

/* Adds the gauge to an existing bus. Does not create the bus. */
esp_err_t passport_cw2017_init(i2c_master_bus_handle_t bus);

/* Reads voltage (mV) and SOC percent. *soc_percent is -1 when not ready. */
esp_err_t passport_cw2017_read(int *millivolts, int *soc_percent);

/*
 * Terminal sleep: write CONFIG=0xF0 and read it back, one retry.
 * ESP_OK when the gauge was never initialized (nothing to do).
 */
esp_err_t passport_cw2017_sleep(void);
#endif
