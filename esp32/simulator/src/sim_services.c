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

#include "sim_services.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"

#include "muse_battery.h"
#include "muse_console.h"
#include "muse_input.h"
#include "muse_menu.h"
#include "muse_settings.h"
#include "muse_settings_ui.h"
#include "muse_voice.h"

#define SIM_DEFAULT_NAME "MuseGadget-SIM001"
#define SIM_DEFAULT_SSID "Muse Simulator"

static muse_wifi_status_t s_wifi = {
    .state = MUSE_WIFI_CONNECTED,
    .ssid = SIM_DEFAULT_SSID,
    .ip = "192.0.2.2",
    .rssi = -45,
};
static muse_ble_status_t s_ble = {
    .state = MUSE_BLE_CONNECTED,
    .secure = true,
    .name = SIM_DEFAULT_NAME,
};
static muse_hatch_status_t s_chat = {
    .state = MUSE_HATCH_REACHABLE,
};
static muse_link_state_t s_link = MUSE_LINK_ONLINE;
static int s_brightness = 100;
static int s_volume = 70;
static int s_mic_gain = 30;
static int s_sleep_s = 120;
static bool s_speaker = true;
static bool s_wifi_on = true;
static bool s_ble_on = false;

static void copy_text(char *out, size_t cap, const char *text)
{
    if (!cap) {
        return;
    }
    if (!text) {
        text = "";
    }
    size_t n = strlen(text);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, text, n);
    out[n] = '\0';
}

void sim_services_reset(void)
{
    s_wifi = (muse_wifi_status_t){
        .state = MUSE_WIFI_CONNECTED,
        .ssid = SIM_DEFAULT_SSID,
        .ip = "192.0.2.2",
        .rssi = -45,
    };
    s_ble = (muse_ble_status_t){
        .state = MUSE_BLE_CONNECTED,
        .secure = true,
        .name = SIM_DEFAULT_NAME,
    };
    s_chat = (muse_hatch_status_t){
        .state = MUSE_HATCH_REACHABLE,
    };
    s_link = MUSE_LINK_ONLINE;
    s_brightness = 100;
    s_volume = 70;
    s_mic_gain = 30;
    s_sleep_s = 120;
    s_speaker = true;
    s_wifi_on = true;
    s_ble_on = false;
}

void sim_services_set_wifi(muse_wifi_state_t state, const char *ssid)
{
    s_wifi.state = state;
    copy_text(s_wifi.ssid, sizeof(s_wifi.ssid), ssid);
    copy_text(s_wifi.ip, sizeof(s_wifi.ip), state == MUSE_WIFI_CONNECTED ? "192.0.2.2" : "");
    s_wifi.rssi = state == MUSE_WIFI_CONNECTED ? -45 : 0;
    s_wifi.detail[0] = '\0';
}

void sim_services_set_ble(muse_ble_state_t state, const char *name, uint32_t passkey)
{
    s_ble.state = state;
    s_ble.passkey = passkey;
    s_ble.secure = state == MUSE_BLE_CONNECTED;
    copy_text(s_ble.name, sizeof(s_ble.name), name);
}

void sim_services_set_paired(bool paired)
{
    if (!paired) {
        s_chat.state = MUSE_HATCH_NOT_SET;
        s_chat.detail[0] = '\0';
    } else if (s_chat.state == MUSE_HATCH_NOT_SET) {
        s_chat.state = MUSE_HATCH_REACHABLE;
    }
}

void sim_services_set_chat_status(muse_hatch_state_t state, const char *detail)
{
    s_chat.state = state;
    copy_text(s_chat.detail, sizeof(s_chat.detail), detail);
}

void sim_services_set_link_state(muse_link_state_t state)
{
    s_link = state;
}

void sim_services_set_brightness(int pct)
{
    if (pct < 10) {
        pct = 10;
    } else if (pct > 100) {
        pct = 100;
    }
    s_brightness = pct;
}

void sim_services_set_speaker(bool on)
{
    s_speaker = on;
}

int muse_settings_brightness(void)
{
    return s_brightness;
}

bool muse_settings_speaker_on(void)
{
    return s_speaker;
}

void muse_settings_set_brightness(int pct)
{
    sim_services_set_brightness(pct);
}

void muse_settings_set_speaker_on(bool on)
{
    sim_services_set_speaker(on);
}

void muse_wifi_status(muse_wifi_status_t *out)
{
    if (out) {
        *out = s_wifi;
    }
}

bool muse_wifi_connected(void)
{
    return s_wifi.state == MUSE_WIFI_CONNECTED;
}

void muse_ble_status(muse_ble_status_t *out)
{
    if (out) {
        *out = s_ble;
    }
}

void muse_hatch_status(muse_hatch_status_t *out)
{
    if (out) {
        *out = s_chat;
    }
}

muse_link_state_t muse_link_state(void)
{
    return s_link;
}

void muse_settings_ui_build(lv_obj_t *tile)
{
    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text(label, "Settings unavailable in preview");
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
}

void muse_settings_ui_tick(bool visible)
{
    (void)visible;
}

bool muse_settings_ui_in_subpage(void)
{
    return false;
}

int muse_settings_volume(void)
{
    return s_volume;
}

void muse_settings_set_volume(int pct)
{
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    s_volume = pct;
}

int muse_settings_mic_gain(void)
{
    return s_mic_gain;
}

void muse_settings_set_mic_gain(int db)
{
    if (db < 0) {
        db = 0;
    } else if (db > 36) {
        db = 36;
    }
    s_mic_gain = db;
}

int muse_settings_sleep_s(void)
{
    return s_sleep_s;
}

void muse_settings_set_sleep_s(int secs)
{
    s_sleep_s = secs < 0 ? 0 : secs;
}

bool muse_settings_wifi_on(void)
{
    return s_wifi_on;
}

void muse_settings_set_wifi_on(bool on)
{
    s_wifi_on = on;
}

bool muse_settings_ble_on(void)
{
    return s_ble_on;
}

void muse_settings_set_ble_on(bool on)
{
    s_ble_on = on;
}

void muse_voice_request_chirp(void)
{
}

void muse_battery_read(muse_battery_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}

bool muse_battery_drain(const muse_battery_t *b, int *rate10, int *full_h)
{
    (void)b;
    (void)rate10;
    (void)full_h;
    return false;
}

const char *muse_link_state_name(muse_link_state_t state)
{
    switch (state) {
    case MUSE_LINK_BOOT: return "Starting";
    case MUSE_LINK_UNPAIRED: return "Ready to pair";
    case MUSE_LINK_PAIRING: return "App connected";
    case MUSE_LINK_CONFIRM: return "Confirm pairing";
    case MUSE_LINK_CONNECTING: return "Connecting";
    case MUSE_LINK_ONLINE: return "Online";
    case MUSE_LINK_OFFLINE: return "Offline";
    case MUSE_LINK_ERROR: return "Error";
    }
    return "";
}

void muse_link_reset_setup(void)
{
}

const char *muse_hatch_state_name(muse_hatch_state_t state)
{
    switch (state) {
    case MUSE_HATCH_NOT_SET: return "Not set up";
    case MUSE_HATCH_OFFLINE: return "Offline";
    case MUSE_HATCH_UNTESTED: return "Saved";
    case MUSE_HATCH_TESTING: return "Connecting";
    case MUSE_HATCH_REACHABLE: return "Connected";
    case MUSE_HATCH_UNREACHABLE: return "Can't connect";
    }
    return "";
}

void muse_input_request_power_off(void)
{
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t desc = {
        .version = "sim",
        .idf_ver = "sim",
    };
    return &desc;
}

void muse_console_write(const void *buf, size_t n)
{
    if (buf && n) {
        (void)fwrite(buf, 1, n, stdout);
        (void)fflush(stdout);
    }
}
