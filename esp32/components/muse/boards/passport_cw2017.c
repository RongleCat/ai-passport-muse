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

#include "passport_cw2017.h"

/* Profile bytes copied from ai-passport-ref bsp_battery.c (MIT). */
const uint8_t passport_cw2017_profile[PASSPORT_CW2017_PROFILE_LEN] = {
    0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAD, 0xC7, 0xC8, 0xCA, 0xBD, 0xB1, 0xC1, 0x94,
    0x88, 0xD1, 0xBD, 0x97, 0x88, 0x66, 0x56, 0x4A,
    0x3F, 0x33, 0x26, 0x5C, 0x37, 0xD1, 0x27, 0xD8,
    0xCC, 0xB7, 0xCF, 0xB3, 0xB2, 0xAE, 0xA6, 0x9E,
    0x99, 0x97, 0x9B, 0x86, 0x47, 0x1E, 0x17, 0x26,
    0x49, 0x96, 0xD9, 0xE1, 0xDD, 0xDC, 0xD4, 0x59,
    0x00, 0x00, 0x90, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5C,
};

_Static_assert(sizeof(passport_cw2017_profile) == PASSPORT_CW2017_PROFILE_LEN,
               "CW2017 profile must be 80 bytes");

int passport_cw2017_raw_to_mv(uint16_t raw)
{
    uint32_t bits = (uint32_t)(raw & 0x3fff);
    return (int)((bits * 3125u) / 10000u);
}

int passport_cw2017_soc_percent(uint8_t soc_integer)
{
    if (soc_integer > 100) {
        return -1;
    }
    return (int)soc_integer;
}

#ifdef ESP_PLATFORM

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cw2017";

#define CW_REG_VERSION 0x00
#define CW_REG_VCELL_H 0x02
#define CW_REG_SOC_H 0x04
#define CW_REG_CONFIG 0x08
#define CW_REG_SOC_ALERT 0x0B
#define CW_REG_PROFILE 0x10

#define CW_CONFIG_ACTIVE 0x00
#define CW_CONFIG_RESTART 0x30
#define CW_CONFIG_SLEEP 0xF0
#define CW_UPDATE_FLAG 0x80

static i2c_master_dev_handle_t s_dev;

static esp_err_t cw_read(uint8_t reg, uint8_t *buf, size_t n)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t cw_write(uint8_t reg, uint8_t val)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t bytes[2] = { reg, val };
    return i2c_master_transmit(s_dev, bytes, sizeof(bytes), 100);
}

static esp_err_t cw_enter_mode(uint8_t mode)
{
    ESP_RETURN_ON_ERROR(cw_write(CW_REG_CONFIG, CW_CONFIG_RESTART), TAG, "restart");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(cw_write(CW_REG_CONFIG, mode), TAG, "mode");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t cw_profile_matches(bool *matches)
{
    uint8_t val = 0;
    *matches = false;
    ESP_RETURN_ON_ERROR(cw_read(CW_REG_SOC_ALERT, &val, 1), TAG, "alert");
    if ((val & CW_UPDATE_FLAG) == 0) {
        return ESP_OK;
    }
    for (size_t i = 0; i < PASSPORT_CW2017_PROFILE_LEN; i++) {
        ESP_RETURN_ON_ERROR(cw_read((uint8_t)(CW_REG_PROFILE + i), &val, 1), TAG, "profile");
        if (val != passport_cw2017_profile[i]) {
            return ESP_OK;
        }
    }
    *matches = true;
    return ESP_OK;
}

static esp_err_t cw_update_profile(void)
{
    uint8_t val = 0;
    ESP_RETURN_ON_ERROR(cw_enter_mode(CW_CONFIG_SLEEP), TAG, "sleep for profile");
    for (size_t i = 0; i < PASSPORT_CW2017_PROFILE_LEN; i++) {
        ESP_RETURN_ON_ERROR(cw_write((uint8_t)(CW_REG_PROFILE + i), passport_cw2017_profile[i]),
                            TAG, "write profile");
    }
    for (size_t i = 0; i < PASSPORT_CW2017_PROFILE_LEN; i++) {
        ESP_RETURN_ON_ERROR(cw_read((uint8_t)(CW_REG_PROFILE + i), &val, 1), TAG, "readback");
        if (val != passport_cw2017_profile[i]) {
            ESP_LOGE(TAG, "profile mismatch at %u: got 0x%02x want 0x%02x",
                     (unsigned)i, val, passport_cw2017_profile[i]);
            return ESP_FAIL;
        }
    }
    ESP_RETURN_ON_ERROR(cw_read(CW_REG_SOC_ALERT, &val, 1), TAG, "alert");
    ESP_RETURN_ON_ERROR(cw_write(CW_REG_SOC_ALERT, val | CW_UPDATE_FLAG), TAG, "update flag");
    return cw_enter_mode(CW_CONFIG_ACTIVE);
}

static esp_err_t cw_wait_soc(void)
{
    for (int try = 0; try < 50; try++) {
        uint8_t soc = 0;
        vTaskDelay(pdMS_TO_TICKS(100));
        if (cw_read(CW_REG_SOC_H, &soc, 1) == ESP_OK && soc <= 100) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t passport_cw2017_init(i2c_master_bus_handle_t bus)
{
    if (s_dev) {
        return ESP_OK;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PASSPORT_CW2017_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t version = 0;
    if (cw_read(CW_REG_VERSION, &version, 1) != ESP_OK) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "CW2017 version 0x%02x", version);

    bool matches = false;
    err = cw_profile_matches(&matches);
    if (err != ESP_OK) {
        goto fail;
    }
    if (!matches) {
        ESP_LOGI(TAG, "writing 520 mAh profile");
        err = cw_update_profile();
        if (err != ESP_OK) {
            goto fail;
        }
    } else {
        uint8_t config = 0;
        err = cw_read(CW_REG_CONFIG, &config, 1);
        if (err != ESP_OK) {
            goto fail;
        }
        if (config != CW_CONFIG_ACTIVE) {
            err = cw_enter_mode(CW_CONFIG_ACTIVE);
            if (err != ESP_OK) {
                goto fail;
            }
        }
    }
    err = cw_wait_soc();
    if (err != ESP_OK) {
        goto fail;
    }
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return err;
}

esp_err_t passport_cw2017_read(int *millivolts, int *soc_percent)
{
    uint8_t cell[2] = { 0 };
    uint8_t soc[2] = { 0 };
    ESP_RETURN_ON_ERROR(cw_read(CW_REG_VCELL_H, cell, 2), TAG, "vcell");
    ESP_RETURN_ON_ERROR(cw_read(CW_REG_SOC_H, soc, 2), TAG, "soc");
    uint16_t raw = (uint16_t)((cell[0] << 8) | cell[1]);
    *millivolts = passport_cw2017_raw_to_mv(raw);
    *soc_percent = passport_cw2017_soc_percent(soc[0]);
    if (*soc_percent < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t passport_cw2017_sleep(void)
{
    if (!s_dev) {
        return ESP_OK;
    }
    for (int attempt = 1; attempt <= 2; attempt++) {
        uint8_t actual = 0;
        esp_err_t wrote = cw_write(CW_REG_CONFIG, CW_CONFIG_SLEEP);
        vTaskDelay(pdMS_TO_TICKS(5));
        esp_err_t read = cw_read(CW_REG_CONFIG, &actual, 1);
        if (wrote == ESP_OK && read == ESP_OK && actual == CW_CONFIG_SLEEP) {
            ESP_LOGI(TAG, "asleep");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "sleep attempt %d failed (actual 0x%02x)", attempt, actual);
        if (attempt == 1) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return ESP_FAIL;
}

#endif /* ESP_PLATFORM */
