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
 * FoloToy AI Passport: three keys on one ADC node.
 * Windows are the half-open millivolt ranges from ai-passport-ref
 * components/bsp/include/bsp_pins.h (MIT):
 *   UP [0, 150), DOWN [150, 447), OK [447, 1900). Released is about 3300 mV.
 * This header is host-compilable: no ESP-IDF types.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Half-open windows, millivolts. max is exclusive. */
#define PASSPORT_KEY_UP_MIN_MV 0
#define PASSPORT_KEY_UP_MAX_MV 150
#define PASSPORT_KEY_DOWN_MIN_MV 150
#define PASSPORT_KEY_DOWN_MAX_MV 447
#define PASSPORT_KEY_OK_MIN_MV 447
#define PASSPORT_KEY_OK_MAX_MV 1900

/* Identical samples required before a new key becomes stable. */
#define PASSPORT_KEY_DEBOUNCE_SAMPLES 3

typedef enum {
    PASSPORT_KEY_NONE = 0,
    PASSPORT_KEY_UP,
    PASSPORT_KEY_DOWN,
    PASSPORT_KEY_OK,
} passport_key_t;

/*
 * Pure decode. millivolts < 0 (a failed ADC read) and anything outside the
 * three windows, including the released level, is PASSPORT_KEY_NONE.
 * Boundaries are half-open, so 150 is DOWN and 447 is OK, never two keys.
 */
passport_key_t passport_key_from_mv(int millivolts);

/*
 * Debounce state. Zero-initialize: stable NONE, pending NONE, count 0.
 * pending/count are the candidate that is not yet stable.
 */
typedef struct {
    passport_key_t stable;
    passport_key_t pending;
    uint8_t count;
} passport_key_debounce_t;

/*
 * One debounce step. pressed/released are PASSPORT_KEY_NONE when that edge
 * did not happen. A direct change from one key to another sets both: released
 * is the old key, pressed is the new one. The first sample of a new candidate
 * counts as 1; it becomes stable on sample PASSPORT_KEY_DEBOUNCE_SAMPLES.
 */
typedef struct {
    passport_key_t stable;
    passport_key_t pressed;
    passport_key_t released;
} passport_key_edges_t;

passport_key_edges_t passport_key_debounce_step(passport_key_debounce_t *state,
                                               passport_key_t sample);

#ifdef __cplusplus
}
#endif
