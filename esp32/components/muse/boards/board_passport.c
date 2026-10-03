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
 * FoloToy AI Passport: ESP32-C3, 8 MB flash, no PSRAM, USB Serial/JTAG.
 * Pins and panel/codec/gauge sequences are from ai-passport-ref (MIT):
 *   components/bsp/include/bsp_pins.h
 *   components/bsp/src/bsp_display.c, bsp_display_lvgl.c, bsp_display_rounding.c
 *   components/bsp/src/bsp_audio.c, bsp_battery.c, bsp_i2c.c
 * Display path follows the Cardputer board (esp_lv_adapter, SPI, MOSI-only).
 * 40 MHz first; the Passport BSP lights this panel at 80 MHz (not tried here).
 * Button hint coordinates are not measured on the hardware.
 */
#include "passport_cw2017.h"
#include "passport_keys.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_W 240
#define LCD_H 320
#define LCD_RADIUS 30
#define DRAW_LINES 20
#define LCD_HOST SPI2_HOST
#define LCD_PCLK_HZ (40 * 1000 * 1000)
#define ADC_SAMPLES 4

#define PIN_LCD_CS GPIO_NUM_1
#define PIN_LCD_DC GPIO_NUM_20
#define PIN_LCD_SCLK GPIO_NUM_8
#define PIN_LCD_MOSI GPIO_NUM_9
#define PIN_LCD_BL GPIO_NUM_21
#define PIN_I2C_SDA GPIO_NUM_10
#define PIN_I2C_SCL GPIO_NUM_7
#define PIN_I2S_MCLK GPIO_NUM_6
#define PIN_I2S_BCLK GPIO_NUM_5
#define PIN_I2S_WS GPIO_NUM_3
#define PIN_I2S_DOUT GPIO_NUM_2
#define PIN_I2S_DIN GPIO_NUM_4
#define PIN_KEYS GPIO_NUM_0

/* Safe levels while the panel is asleep: CS high, clocks and data and DC and backlight low. */
static const gpio_num_t s_lcd_pins[] = {
    PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI, PIN_LCD_DC, PIN_LCD_BL,
};
static const uint8_t s_lcd_levels[] = { 1, 0, 0, 0, 0 };

/* ST7789P3 porch/power/gamma from the panel vendor, via bsp_display.c (MIT).
 * SLPOUT, COLMOD, INVON, DISPON and MADCTL are sent by esp_lcd, not here. */
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;
    uint16_t delay_ms;
} st_cmd_t;

static const st_cmd_t s_st7789p3[] = {
    { 0xB2, { 0x05, 0x05, 0x00, 0x33, 0x33 }, 5, 0 },
    { 0xB7, { 0x35 }, 1, 0 },
    { 0xBB, { 0x21 }, 1, 0 },
    { 0xC0, { 0x2C }, 1, 0 },
    { 0xC2, { 0x01 }, 1, 0 },
    { 0xC3, { 0x0B }, 1, 0 },
    { 0xC4, { 0x20 }, 1, 0 },
    { 0xC6, { 0x0F }, 1, 0 },
    { 0xD0, { 0xA7, 0xA1 }, 2, 0 },
    { 0xD0, { 0xA4, 0xA1 }, 2, 0 },
    { 0xD6, { 0xA1 }, 1, 0 },
    { 0xE0, { 0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
              0x49, 0x09, 0x16, 0x15, 0x26, 0x2B }, 14, 0 },
    { 0xE1, { 0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
              0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A }, 14, 10 },
};

/* ES8311 force-sleep, from bsp_audio.c (MIT). REG45=0x01 turns off the
 * BCLK/LRCK internal pull-ups. REG0E bit 7 is undefined and may read 0. */
static const uint8_t s_es8311_sleep[][2] = {
    { 0x32, 0x00 }, { 0x17, 0x00 }, { 0x0E, 0xFF }, { 0x12, 0x02 },
    { 0x14, 0x00 }, { 0x0D, 0xFA }, { 0x15, 0x00 }, { 0x02, 0x10 },
    { 0x00, 0x00 }, { 0x00, 0x1F }, { 0x01, 0x30 }, { 0x01, 0x00 },
    { 0x45, 0x01 }, { 0x0D, 0xFC }, { 0x02, 0x00 },
};
static const uint8_t s_es8311_check[][3] = {
    { 0x00, 0x1F, 0xFF }, { 0x01, 0x00, 0xFF }, { 0x0D, 0xFC, 0xFF },
    { 0x0E, 0x7F, 0x7F }, { 0x12, 0x02, 0xFF }, { 0x45, 0x01, 0xFF },
};

static i2c_master_bus_handle_t s_i2c;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static bool s_bl_ready;
static i2s_chan_handle_t s_tx, s_rx;
static const audio_codec_ctrl_if_t *s_ctrl;
static esp_codec_dev_handle_t s_spk, s_mic;
static passport_key_debounce_t s_keys;

static int32_t clamp_radius(int32_t radius)
{
    const int32_t max_radius = LCD_W < LCD_H ? LCD_W / 2 : LCD_H / 2;
    if (radius <= 0) {
        return 0;
    }
    return radius > max_radius ? max_radius : radius;
}

/* Visible x span of one row inside a radius-px rounded rectangle.
 * From bsp_display_rounding.c (MIT). */
static bool rounded_row_span(int32_t y, int32_t radius, int32_t *x1, int32_t *x2)
{
    if (y < 0 || y >= LCD_H) {
        return false;
    }
    radius = clamp_radius(radius);
    if (radius <= 0 || (y >= radius && y < LCD_H - radius)) {
        *x1 = 0;
        *x2 = LCD_W - 1;
        return true;
    }
    const int32_t edge_y = y < radius ? radius - y : y - (LCD_H - 1 - radius);
    int32_t inset = 0;
    while ((inset + 1) * (inset + 1) + edge_y * edge_y <= radius * radius) {
        ++inset;
    }
    *x1 = radius - inset;
    *x2 = LCD_W - radius + inset - 1;
    if (*x1 < 0) {
        *x1 = 0;
    }
    if (*x2 >= LCD_W) {
        *x2 = LCD_W - 1;
    }
    return *x1 <= *x2;
}

/* Paint pixels outside the 30 px corners black. Black is 0 in either RGB565
 * byte order, so this is safe before or after the adapter's byte swap. */
static void rounded_flush_event(lv_event_t *event)
{
    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *draw = lv_display_get_buf_active(disp);
    if (!area || !draw || !draw->data ||
        lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565) {
        return;
    }
    const int32_t width = lv_area_get_width(area);
    if (draw->header.stride < (uint32_t)width * sizeof(uint16_t)) {
        return;
    }
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        uint16_t *row = (uint16_t *)(draw->data + (y - area->y1) * draw->header.stride);
        int32_t visible_x1, visible_x2;
        if (!rounded_row_span(y, LCD_RADIUS, &visible_x1, &visible_x2)) {
            memset(row, 0, (size_t)width * sizeof(uint16_t));
            continue;
        }
        const int32_t clear_left_end = visible_x1 > area->x2 ? area->x2 : visible_x1 - 1;
        const int32_t clear_right_start = visible_x2 < area->x1 ? area->x1 : visible_x2 + 1;
        for (int32_t x = area->x1; x <= clear_left_end; ++x) {
            row[x - area->x1] = 0;
        }
        for (int32_t x = clear_right_start; x <= area->x2; ++x) {
            row[x - area->x1] = 0;
        }
    }
}

static esp_err_t lcd_set_safe_levels(void)
{
    esp_err_t first = ESP_OK;
    for (size_t i = 0; i < sizeof(s_lcd_pins) / sizeof(s_lcd_pins[0]); i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_lcd_pins[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err == ESP_OK) {
            err = gpio_set_level(s_lcd_pins[i], s_lcd_levels[i]);
        }
        if (err != ESP_OK && first == ESP_OK) {
            first = err;
        }
    }
    return first;
}

static void release_sleep_holds(void)
{
    gpio_deep_sleep_hold_dis();
    lcd_set_safe_levels();
    for (size_t i = 0; i < sizeof(s_lcd_pins) / sizeof(s_lcd_pins[0]); i++) {
        gpio_hold_dis(s_lcd_pins[i]);
    }
    gpio_hold_dis(PIN_KEYS);
}

static int read_key_mv(void)
{
    if (!s_adc || !s_cali) {
        return -1;
    }
    int sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw) != ESP_OK) {
            return -1;
        }
        sum += raw;
    }
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_cali, sum / ADC_SAMPLES, &mv) != ESP_OK) {
        return -1;
    }
    return mv;
}

static esp_err_t keys_init(void)
{
    /* External 10 kΩ pull-up. An internal pull (~45 kΩ) collapses the ladder. */
    gpio_pullup_dis(PIN_KEYS);
    gpio_pulldown_dis(PIN_KEYS);
    const adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t channel = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &channel);
    if (err != ESP_OK) {
        return err;
    }
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_0,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC calibration failed; refusing to guess a key");
        return err;
    }
    return ESP_OK;
}

static void heap_stage(const char *label)
{
#if CONFIG_MUSE_PASSPORT_HEAP_LOG
    ESP_LOGI(TAG, "HEAP %-22s free=%u min=%u largest=%u", label,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#else
    (void)label;
#endif
}

static const char *key_name(passport_key_t key)
{
    switch (key) {
    case PASSPORT_KEY_UP:
        return "UP";
    case PASSPORT_KEY_DOWN:
        return "DOWN";
    case PASSPORT_KEY_OK:
        return "OK";
    default:
        return "NONE";
    }
}

#if CONFIG_MUSE_PASSPORT_HEAP_LOG
static void heap_log_task(void *arg)
{
    (void)arg;
    for (;;) {
        ESP_LOGI(TAG, "internal heap free=%u min=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
#endif

static esp_err_t init(void)
{
    release_sleep_holds();
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    esp_err_t batt = passport_cw2017_init(s_i2c);
    if (batt != ESP_OK) {
        ESP_LOGW(TAG, "CW2017 unavailable (%s); battery meter off", esp_err_to_name(batt));
    }
    ESP_RETURN_ON_ERROR(keys_init(), TAG, "keys");
#if CONFIG_MUSE_PASSPORT_HEAP_LOG
    if (xTaskCreate(heap_log_task, "pass_heap", 2560, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "heap log task not started");
    }
#endif
    return ESP_OK;
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");
    s_bl_ready = true;
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;
    if (backlight_init() != ESP_OK) {
        return NULL;
    }
    const spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_W * DRAW_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC, /* RST is tied high; reset() sends SWRESET */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    if (esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_st7789p3) / sizeof(s_st7789p3[0]); i++) {
        const st_cmd_t *cmd = &s_st7789p3[i];
        if (esp_lcd_panel_io_tx_param(s_io, cmd->cmd, cmd->data, cmd->len) != ESP_OK) {
            ESP_LOGE(TAG, "vendor cmd 0x%02x failed", cmd->cmd);
            return NULL;
        }
        if (cmd->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }
    if (esp_lcd_panel_invert_color(s_panel, true) != ESP_OK ||
        esp_lcd_panel_mirror(s_panel, false, false) != ESP_OK ||
        esp_lcd_panel_set_gap(s_panel, 0, 0) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        return NULL;
    }

    heap_stage("after panel");
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    /* One core: keep drawing below Wi-Fi and app_main, as on the C6 board. */
    adapter_cfg.task_priority = 1;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_W,
            .ver_res = LCD_H,
            .buffer_height = DRAW_LINES,
            .use_psram = false,
            .require_double_buffer = false,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) {
        return NULL;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return NULL;
    }
    lv_display_add_event_cb(disp, rounded_flush_event, LV_EVENT_FLUSH_START, NULL);
    esp_lv_adapter_unlock();
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    heap_stage("after display");
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    if (!s_bl_ready) {
        return;
    }
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    if (s_panel) {
        esp_lcd_panel_disp_sleep(s_panel, sleep);
    }
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    /* 0x30 is the 8-bit form of the 7-bit address 0x18. */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c,
    };
    s_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && s_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = s_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = GPIO_NUM_NC,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
        .no_dac_ref = true, /* mono mic must use the ADC, not the DAC reference */
        .mclk_div = 256,
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    s_spk = esp_codec_dev_new(&out_cfg);
    s_mic = esp_codec_dev_new(&in_cfg);
    *spk = s_spk;
    *mic = s_mic;
    return s_spk && s_mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    int mv = read_key_mv();
    passport_key_t before = s_keys.stable;
    passport_key_edges_t edges = passport_key_debounce_step(&s_keys, passport_key_from_mv(mv));
    /* Debounce already limits this to a real stable change. */
    if (s_keys.stable != before) {
        ESP_LOGI(TAG, "key mv=%d name=%s", mv, key_name(s_keys.stable));
    }
    unsigned ev = 0;
    if (edges.released == PASSPORT_KEY_OK) {
        ev |= MUSE_BTN_TALK_RELEASE;
    } else if (edges.released == PASSPORT_KEY_DOWN) {
        ev |= MUSE_BTN_AUX_RELEASE;
    }
    if (edges.pressed == PASSPORT_KEY_OK) {
        ev |= MUSE_BTN_TALK_PRESS;
    } else if (edges.pressed == PASSPORT_KEY_DOWN) {
        ev |= MUSE_BTN_AUX_PRESS;
    } else if (edges.pressed == PASSPORT_KEY_UP) {
        ev |= MUSE_BTN_UP;
    }
    return ev;
}

/* GPIO0 stays an ADC pin, so this cannot arm a GPIO wake without giving the
 * ADC up (doing that made Passport wake at once; see the reference notes).
 * Delay in slices so tickless idle can light-sleep, and return when the
 * decoded key changes or the task is notified. */
static void wait_buttons(int timeout_ms)
{
    if (timeout_ms <= 0) {
        return;
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    passport_key_t seen = s_keys.stable;
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            return;
        }
        if (passport_key_from_mv(read_key_mv()) != seen) {
            return;
        }
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            return;
        }
        TickType_t slice = pdMS_TO_TICKS(20);
        if (slice > deadline - now) {
            slice = deadline - now;
        }
        if (ulTaskNotifyTake(pdTRUE, slice) > 0) {
            return;
        }
    }
}

static esp_err_t read_power(muse_power_t *out)
{
    int mv = 0, pct = 0;
    ESP_RETURN_ON_ERROR(passport_cw2017_read(&mv, &pct), TAG, "battery");
    *out = (muse_power_t){
        .battery_pct = pct,
        .battery_mv = mv,
        .charging = false,
        .usb = false,
    };
    return ESP_OK;
}

static esp_err_t i2s_quiesce(void)
{
    esp_err_t first = ESP_OK;
    i2s_chan_handle_t channels[] = { s_tx, s_rx };
    for (size_t i = 0; i < 2; i++) {
        if (!channels[i]) {
            continue;
        }
        esp_err_t err = i2s_channel_disable(channels[i]);
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;
        }
        if (err != ESP_OK && first == ESP_OK) {
            first = err;
        }
    }
    return first;
}

static esp_err_t es8311_force_sleep_once(void)
{
    if (!s_ctrl || !s_ctrl->write_reg || !s_ctrl->read_reg) {
        return ESP_ERR_INVALID_STATE;
    }
    bool ok = true;
    for (size_t i = 0; i < sizeof(s_es8311_sleep) / sizeof(s_es8311_sleep[0]); i++) {
        uint8_t value = s_es8311_sleep[i][1];
        int wrote = s_ctrl->write_reg(s_ctrl, s_es8311_sleep[i][0], 1, &value, 1);
        if (wrote != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "ES8311 REG%02X write failed (%d)", s_es8311_sleep[i][0], wrote);
            ok = false;
        }
    }
    for (size_t i = 0; i < sizeof(s_es8311_check) / sizeof(s_es8311_check[0]); i++) {
        uint8_t actual = 0;
        int read = s_ctrl->read_reg(s_ctrl, s_es8311_check[i][0], 1, &actual, 1);
        uint8_t mask = s_es8311_check[i][2];
        if (read != ESP_CODEC_DEV_OK || (actual & mask) != (s_es8311_check[i][1] & mask)) {
            ESP_LOGE(TAG, "ES8311 REG%02X check failed (got 0x%02x)", s_es8311_check[i][0], actual);
            ok = false;
        }
    }
    return ok ? ESP_OK : ESP_FAIL;
}

static void pins_hiz(uint64_t mask)
{
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "pin release failed");
    }
}

static void note(const char *step, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power off continues after %s: %s", step, esp_err_to_name(err));
    }
}

static esp_err_t power_off(void)
{
    /* Peripheral failures are logged and do not abort. Pin release is terminal:
     * if deep sleep returns, restart instead of touching the bus again. */
    display_pause(true);
    set_brightness(0);
    note("CW2017", passport_cw2017_sleep());
    if (s_spk) {
        esp_codec_dev_close(s_spk);
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
    }
    esp_err_t slept = es8311_force_sleep_once();
    if (slept != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(5));
        slept = es8311_force_sleep_once();
    }
    note("ES8311", slept);
    note("I2S", i2s_quiesce());
    pins_hiz((1ULL << PIN_I2S_MCLK) | (1ULL << PIN_I2S_BCLK) | (1ULL << PIN_I2S_WS) |
             (1ULL << PIN_I2S_DOUT) | (1ULL << PIN_I2S_DIN));
    pins_hiz((1ULL << PIN_I2C_SDA) | (1ULL << PIN_I2C_SCL));

    if (s_panel) {
        note("display off", esp_lcd_panel_disp_on_off(s_panel, false));
        note("display sleep", esp_lcd_panel_disp_sleep(s_panel, true));
    }
    if (s_bl_ready) {
        note("backlight", ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0));
    }
    note("lcd levels", lcd_set_safe_levels());
    for (size_t i = 0; i < sizeof(s_lcd_pins) / sizeof(s_lcd_pins[0]); i++) {
        note("lcd hold", gpio_hold_en(s_lcd_pins[i]));
    }
    gpio_deep_sleep_hold_en();

    if (s_cali) {
        adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
    }
    if (s_adc) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
    }
    gpio_config_t key = {
        .pin_bit_mask = 1ULL << PIN_KEYS,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&key);
    /* A key held down already looks like the wake level. Wait it out.
     * The digital threshold versus the OK/DOWN ladder is not verified. */
    for (int i = 0; i < 100 && gpio_get_level(PIN_KEYS) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (gpio_get_level(PIN_KEYS) == 0) {
        ESP_LOGW(TAG, "GPIO0 still low; deep sleep may wake immediately");
    }
    esp_err_t wake = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
        1ULL << PIN_KEYS, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake != ESP_OK) {
        ESP_LOGE(TAG, "GPIO0 wake not armed (%s); staying on", esp_err_to_name(wake));
        return wake;
    }
    esp_deep_sleep_start();
    esp_restart();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "FoloToy AI Passport",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = false,
    .diagonal_in = 2.4f,
    .keyboard = false,
    .talk_button = "OK",
    .aux_button = "DOWN",
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 36, -8 },
    .aux_hint = { LV_ALIGN_BOTTOM_MID, -36, -8 },
    .frame_ms = 50,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
