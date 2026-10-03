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

/* Host driver for muse_hatch_caption_at and muse_text_to_ascii.
 *   page    stdin is one request: five little-endian int32 (wide, cols, lines,
 *           at, nbytes) then nbytes of text. stdout is int32 length then the
 *           page, or length -1 when there is no page.
 *   keep    stdin is text. stdout is that text after muse_text_to_ascii.
 * wide is ignored unless this file is built with MUSE_CJK_COLS_TOGGLE. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_chat_priv.h"
#include "muse_text.h"

static int s_cols = 16;
static int s_lines = 2;

void muse_state_page(int *cols, int *lines)
{
    *cols = s_cols;
    *lines = s_lines;
}

#ifdef MUSE_CJK_COLS_TOGGLE
void muse_cjk_cols_test_set(int on);
#endif

static int read_full(void *buf, size_t n)
{
    unsigned char *p = buf;
    while (n) {
        size_t got = fread(p, 1, n, stdin);
        if (!got) {
            return -1;
        }
        p += got;
        n -= got;
    }
    return 0;
}

static char *read_all(size_t *len)
{
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap + 1);
    size_t got;
    while (buf && (got = fread(buf + n, 1, cap - n, stdin)) > 0) {
        n += got;
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap + 1);
        }
    }
    if (!buf) {
        return NULL;
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }
    if (!strcmp(argv[1], "keep")) {
        size_t n = 0;
        char *in = read_all(&n);
        char buf[4096];
        if (!in || n >= sizeof(buf)) {
            return 2;
        }
        memcpy(buf, in, n + 1);
        free(in);
        muse_text_to_ascii(buf, sizeof(buf));
        fwrite(buf, 1, strlen(buf), stdout);
        return 0;
    }
    if (strcmp(argv[1], "page") != 0) {
        return 2;
    }
    int32_t hdr[5];
    if (read_full(hdr, sizeof(hdr)) != 0 || hdr[4] < 0 || hdr[4] > 100000) {
        return 2;
    }
    char *text = calloc((size_t)hdr[4] + 1, 1);
    if (!text || (hdr[4] && read_full(text, (size_t)hdr[4]) != 0)) {
        return 2;
    }
#ifdef MUSE_CJK_COLS_TOGGLE
    muse_cjk_cols_test_set(hdr[0]);
#else
    if (hdr[0]) {
        fprintf(stderr, "this build has no wide-column switch\n");
        return 2;
    }
#endif
    s_cols = hdr[1];
    s_lines = hdr[2];
    char out[4096];
    bool ok = muse_hatch_caption_at(text, hdr[3] < 0 ? 0 : (size_t)hdr[3], out, sizeof(out));
    int32_t n = ok ? (int32_t)strlen(out) : -1;
    fwrite(&n, sizeof(n), 1, stdout);
    if (ok) {
        fwrite(out, 1, (size_t)n, stdout);
    }
    free(text);
    return 0;
}
