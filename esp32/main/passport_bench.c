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

/* Passport serial benches. Nothing here runs unless the console asks.
 * >fit measures the pairing decrypt heap shape. >nvstest, when
 * CONFIG_MUSE_PASSPORT_NVS_STRESS is on, commits NVS while the radios
 * and the mic are already up. Neither command stores a credential. */

#include "passport_bench.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "ble_server.h"
#include "muse_audio.h"
#include "muse_voice.h"
#include "wifi_mgr.h"

void passport_i2s_stats(const char *tag);
void passport_i2s_stats_reset(void);

static atomic_bool s_busy;

static size_t largest_block(void) {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

static void heap_line(const char *step) {
    printf("@fit %s free=%u min=%u largest=%u\n", step,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)largest_block());
    fflush(stdout);
}

/* Largest free block after the current largest one is taken away. */
static size_t second_largest(void) {
    size_t first = largest_block();
    if (first < 16) return 0;
    void *held = malloc(first);
    size_t second = held ? largest_block() : 0;
    free(held);
    return second;
}

static char *outer_message(size_t *b64_len) {
    const char *prefix = "{\"action\":\"pairing_encrypted\",\"ciphertext\":\"";
    const char *suffix = "\"}";
    const size_t total = 2783;
    size_t pre = strlen(prefix);
    size_t fill = total - pre - strlen(suffix);
    char *msg = malloc(total + 1);
    if (!msg) return NULL;
    memcpy(msg, prefix, pre);
    memset(msg + pre, 'A', fill);
    memcpy(msg + pre + fill, suffix, strlen(suffix));
    msg[total] = '\0';
    *b64_len = fill;
    return msg;
}

static char *inner_message(size_t *len_out) {
    char *msg = malloc(2200);
    if (!msg) return NULL;
    int head = snprintf(msg, 2200,
                        "{\"action\":\"provision_v2\",\"ssid\":\"net\","
                        "\"password\":\"secret\",\"access_token\":\"");
    if (head < 0 || head > 200) {
        free(msg);
        return NULL;
    }
    memset(msg + head, 'A', 1800);
    strcpy(msg + head + 1800,
           "\",\"refresh_token\":\"r\",\"token_type\":\"device\"}");
    *len_out = strlen(msg);
    return msg;
}

static void measure_parse(const char *label, const char *msg, size_t len) {
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    cJSON *root = cJSON_ParseWithLength(msg, len);
    size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t used = before > after ? before - after : 0;
    printf("@fit cjson %s len=%u bytes=%u ok=%d\n", label, (unsigned)len,
           (unsigned)used, root != NULL);
    fflush(stdout);
    cJSON_Delete(root);
}

/* Firmware order for one provision submit: free the RX buffer after the
 * outer parse, sized decrypt buffers, drop the envelope, parse the inner
 * JSON, free that plaintext, copy the credential strings, drop the inner
 * JSON, then take the 8192-byte provision stack. */
static void run_new_path(const char *tag, const char *outer, size_t b64_len,
                         const char *inner, size_t inner_len) {
    size_t n = b64_len ? (b64_len * 3) / 4 + 3 : 0;
    void *rx = malloc(8192);
    if (rx) {
        void *shrunk = realloc(rx, 2783);
        if (shrunk) rx = shrunk;
    }
    cJSON *outer_json = outer ? cJSON_ParseWithLength(outer, 2783) : NULL;
    free(rx);
    size_t at_decrypt = largest_block();
    void *cipher = n ? malloc(n) : NULL;
    void *plain = n ? malloc(n + 1) : NULL;
    void *b64_tmp = b64_len ? malloc(b64_len + 4) : NULL;
    int cipher_ok = cipher != NULL;
    int plain_ok = plain != NULL;
    int b64_ok = b64_tmp != NULL;
    free(b64_tmp);
    free(cipher);
    cJSON_Delete(outer_json);
    cJSON *inner_json = inner ? cJSON_ParseWithLength(inner, inner_len) : NULL;
    int inner_ok = inner_json != NULL;
    free(plain);
    void *dup_ssid = malloc(33);
    void *dup_pass = malloc(64);
    void *dup_token = malloc(1801);
    void *dup_refresh = malloc(32);
    void *dup_url = malloc(48);
    void *args = calloc(1, 64);
    int dups_ok = dup_ssid && dup_pass && dup_token && dup_refresh && dup_url && args;
    cJSON_Delete(inner_json);
    size_t before_stack = largest_block();
    size_t free_before_stack = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    void *prov_stack = malloc(8192);
    printf("@fit %s n=%u at_decrypt=%u cipher=%d plain=%d b64tmp=%d inner=%d "
           "dups=%d before_stack=%u free_before_stack=%u stack=%d\n",
           tag, (unsigned)n, (unsigned)at_decrypt, cipher_ok, plain_ok, b64_ok,
           inner_ok, dups_ok, (unsigned)before_stack,
           (unsigned)free_before_stack, prov_stack != NULL);
    fflush(stdout);
    free(prov_stack);
    free(args);
    free(dup_url);
    free(dup_refresh);
    free(dup_token);
    free(dup_pass);
    free(dup_ssid);
}

/* Leave one free block near `target` by holding every other block that
 * could satisfy an 8192-byte alloc. */
static int hold_down_to(void **holds, int cap, size_t target) {
    int n = 0;
    for (int guard = 0; guard < 8 && n < cap; guard++) {
        size_t first = largest_block();
        size_t second = second_largest();
        if (second >= 8192 && first >= 16) {
            void *aside = malloc(first);
            void *extra = aside ? malloc(second) : NULL;
            free(aside);
            if (!extra) break;
            holds[n++] = extra;
            continue;
        }
        if (first > target + 256) {
            size_t take = first - target - 48;
            void *trim = malloc(take);
            if (!trim) break;
            holds[n++] = trim;
        }
        break;
    }
    return n;
}

static void fit_task(void *arg) {
    (void)arg;
    size_t min_at_start = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    heap_line("start");
    printf("@fit second=%u adv=%d wifi=%d voice=%d\n",
           (unsigned)second_largest(),
           ble_server_is_advertising(),
           wifi_mgr_is_connected(),
           xTaskGetHandle("muse_voice") != NULL);
    fflush(stdout);

    size_t record = sizeof(wifi_ap_record_t);
    void *recs = calloc(40, record);
    printf("@fit scan_records bytes=%u ok=%d\n", (unsigned)(40 * record),
           recs != NULL);
    fflush(stdout);
    free(recs);

    size_t b64_len = 0;
    char *outer = outer_message(&b64_len);
    size_t inner_len = 0;
    char *inner = inner_message(&inner_len);
    if (outer) measure_parse("outer", outer, 2783);
    if (inner) measure_parse("inner", inner, inner_len);

    size_t natural_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    run_new_path("natural", outer, b64_len, inner, inner_len);
    size_t natural_after = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    printf("@fit natural_min %u->%u\n", (unsigned)natural_min, (unsigned)natural_after);
    fflush(stdout);

    void *holds[8] = { 0 };
    int held = hold_down_to(holds, 8, 18432);
    printf("@fit trimmed holds=%d largest=%u second=%u\n", held,
           (unsigned)largest_block(), (unsigned)second_largest());
    fflush(stdout);

    void *rx = malloc(8192);
    size_t after_rx = largest_block();
    cJSON *kept = outer ? cJSON_ParseWithLength(outer, 2783) : NULL;
    void *old_cipher = malloc(8192);
    void *old_plain = malloc(8193);
    printf("@fit old rx=%d after_rx_largest=%u json=%d cipher=%d plain=%d\n",
           rx != NULL, (unsigned)after_rx, kept != NULL,
           old_cipher != NULL, old_plain != NULL);
    fflush(stdout);
    free(old_plain);
    free(old_cipher);
    cJSON_Delete(kept);
    free(rx);
    run_new_path("shaped18432", outer, b64_len, inner, inner_len);
    for (int i = 0; i < held; i++) free(holds[i]);

    memset(holds, 0, sizeof(holds));
    held = hold_down_to(holds, 8, 14336);
    printf("@fit trimmed14336 holds=%d largest=%u second=%u\n", held,
           (unsigned)largest_block(), (unsigned)second_largest());
    fflush(stdout);
    run_new_path("shaped14336", outer, b64_len, inner, inner_len);
    for (int i = 0; i < held; i++) free(holds[i]);
    free(inner);
    free(outer);

    heap_line("done");
    printf("@fit min_delta=%u\n",
           (unsigned)(min_at_start
                      - heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    fflush(stdout);
    atomic_store(&s_busy, false);
    vTaskDelete(NULL);
}

void passport_bench_fit(void) {
    if (atomic_exchange(&s_busy, true)) {
        printf("@fit busy\n");
        fflush(stdout);
        return;
    }
    if (xTaskCreate(fit_task, "pair_fit", 4096, NULL, 5, NULL) != pdPASS) {
        atomic_store(&s_busy, false);
        printf("@fit spawn fail\n");
        fflush(stdout);
    }
}

#if CONFIG_MUSE_PASSPORT_NVS_STRESS
static void nvs_stress_task(void *arg) {
    (void)arg;
    uint32_t reads0 = muse_audio_read_failures();
    bool adv0 = ble_server_is_advertising();
    bool wifi0 = wifi_mgr_is_connected();
    bool conn0 = ble_server_has_connection();
    bool voice0 = xTaskGetHandle("muse_voice") != NULL;
    int64_t t0 = esp_timer_get_time();
    size_t min0 = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    nvs_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    nvs_get_stats(NULL, &stats);
    size_t avail0 = stats.available_entries;
    size_t avail_min = stats.available_entries;
    int reclaims = 0;
    printf("@nvstest start adv=%d wifi=%d conn=%d voice=%d "
           "avail=%u used=%u total=%u\n",
           adv0, wifi0, conn0, voice0,
           (unsigned)stats.available_entries, (unsigned)stats.used_entries,
           (unsigned)stats.total_entries);
    fflush(stdout);
    passport_i2s_stats_reset();
    muse_voice_request_selftest();

    nvs_handle_t handle;
    esp_err_t err = nvs_open("nvstress", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        printf("@nvstest open %s\n", esp_err_to_name(err));
        fflush(stdout);
        atomic_store(&s_busy, false);
        vTaskDelete(NULL);
        return;
    }
    uint8_t *buf = malloc(3000);
    if (!buf) {
        nvs_close(handle);
        printf("@nvstest nomem\n");
        fflush(stdout);
        atomic_store(&s_busy, false);
        vTaskDelete(NULL);
        return;
    }
    static const size_t sizes[] = { 32, 128, 512, 1536, 3000 };
    int ok = 0;
    int fail = 0;
    size_t prev_avail = stats.available_entries;
    for (int i = 0; i < 200; i++) {
        if (i % 8 == 0) {
            muse_voice_request_chirp();
        }
        size_t n = sizes[i % 5];
        memset(buf, (uint8_t)i, n);
        err = nvs_set_blob(handle, "blob", buf, n);
        if (err == ESP_OK) err = nvs_commit(handle);
        if (err == ESP_OK) {
            ok++;
        } else {
            fail++;
            printf("@nvstest fail i=%d %s\n", i, esp_err_to_name(err));
            fflush(stdout);
            if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
                nvs_erase_all(handle);
                nvs_commit(handle);
            }
        }
        if (nvs_get_stats(NULL, &stats) == ESP_OK) {
            if (stats.available_entries < avail_min) {
                avail_min = stats.available_entries;
            }
            if (stats.available_entries > prev_avail) reclaims++;
            prev_avail = stats.available_entries;
        }
        if ((i + 1) % 50 == 0) {
            printf("@nvstest progress %d ok=%d fail=%d adv=%d wifi=%d "
                   "voice=%d read_fail=%u avail=%u min=%u\n",
                   i + 1, ok, fail,
                   ble_server_is_advertising(),
                   wifi_mgr_is_connected(),
                   xTaskGetHandle("muse_voice") != NULL,
                   (unsigned)(muse_audio_read_failures() - reads0),
                   (unsigned)stats.available_entries,
                   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
            fflush(stdout);
        }
    }
    nvs_erase_all(handle);
    esp_err_t erased = nvs_commit(handle);
    nvs_close(handle);
    free(buf);
    nvs_get_stats(NULL, &stats);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    printf("@nvstest done ok=%d fail=%d erase=%s reclaims=%d "
           "avail %u->%u (min %u) adv %d->%d wifi %d->%d conn %d->%d "
           "voice %d->%d read_fail=%u heap_min %u->%u ms=%lld\n",
           ok, fail, esp_err_to_name(erased), reclaims,
           (unsigned)avail0, (unsigned)stats.available_entries,
           (unsigned)avail_min,
           adv0, ble_server_is_advertising(),
           wifi0, wifi_mgr_is_connected(),
           conn0, ble_server_has_connection(),
           voice0, xTaskGetHandle("muse_voice") != NULL,
           (unsigned)(muse_audio_read_failures() - reads0),
           (unsigned)min0,
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (long long)ms);
    /* The start available count was printed on the start line. */
    fflush(stdout);
    passport_i2s_stats("nvstest");
    atomic_store(&s_busy, false);
    vTaskDelete(NULL);
}
#endif

void passport_bench_nvs_stress(void) {
#if CONFIG_MUSE_PASSPORT_NVS_STRESS
    if (atomic_exchange(&s_busy, true)) {
        printf("@nvstest busy\n");
        fflush(stdout);
        return;
    }
    if (xTaskCreate(nvs_stress_task, "nvs_stress", 4096, NULL, 5, NULL) != pdPASS) {
        atomic_store(&s_busy, false);
        printf("@nvstest spawn fail\n");
        fflush(stdout);
    }
#else
    printf("@nvstest off\n");
    fflush(stdout);
#endif
}
