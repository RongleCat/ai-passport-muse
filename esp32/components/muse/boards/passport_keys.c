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

#include "passport_keys.h"

passport_key_t passport_key_from_mv(int millivolts)
{
    if (millivolts >= PASSPORT_KEY_UP_MIN_MV && millivolts < PASSPORT_KEY_UP_MAX_MV) {
        return PASSPORT_KEY_UP;
    }
    if (millivolts >= PASSPORT_KEY_DOWN_MIN_MV && millivolts < PASSPORT_KEY_DOWN_MAX_MV) {
        return PASSPORT_KEY_DOWN;
    }
    if (millivolts >= PASSPORT_KEY_OK_MIN_MV && millivolts < PASSPORT_KEY_OK_MAX_MV) {
        return PASSPORT_KEY_OK;
    }
    return PASSPORT_KEY_NONE;
}

passport_key_edges_t passport_key_debounce_step(passport_key_debounce_t *state,
                                               passport_key_t sample)
{
    passport_key_edges_t out = {
        .stable = state->stable,
        .pressed = PASSPORT_KEY_NONE,
        .released = PASSPORT_KEY_NONE,
    };
    if (sample == state->stable) {
        state->pending = sample;
        state->count = 0;
        return out;
    }
    if (sample != state->pending) {
        state->pending = sample;
        state->count = 1;
        return out;
    }
    if (++state->count < PASSPORT_KEY_DEBOUNCE_SAMPLES) {
        return out;
    }
    if (state->stable != PASSPORT_KEY_NONE) {
        out.released = state->stable;
    }
    state->stable = sample;
    state->count = 0;
    out.stable = sample;
    if (sample != PASSPORT_KEY_NONE) {
        out.pressed = sample;
    }
    return out;
}
