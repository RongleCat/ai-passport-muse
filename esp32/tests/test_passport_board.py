# SPDX-License-Identifier: Apache-2.0

"""Host tests for the FoloToy AI Passport pure board helpers."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BOARDS = ROOT / "components" / "muse" / "boards"

HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "passport_cw2017.h"
#include "passport_keys.h"

static passport_key_edges_t step(passport_key_debounce_t *state, int millivolts)
{
    return passport_key_debounce_step(state, passport_key_from_mv(millivolts));
}

static void assert_no_edges(passport_key_edges_t edges, passport_key_t stable)
{
    assert(edges.stable == stable);
    assert(edges.pressed == PASSPORT_KEY_NONE);
    assert(edges.released == PASSPORT_KEY_NONE);
}

static void test_windows(void)
{
    assert(passport_key_from_mv(-1) == PASSPORT_KEY_NONE);
    assert(passport_key_from_mv(0) == PASSPORT_KEY_UP);
    assert(passport_key_from_mv(149) == PASSPORT_KEY_UP);
    assert(passport_key_from_mv(150) == PASSPORT_KEY_DOWN);
    assert(passport_key_from_mv(446) == PASSPORT_KEY_DOWN);
    assert(passport_key_from_mv(447) == PASSPORT_KEY_OK);
    assert(passport_key_from_mv(1899) == PASSPORT_KEY_OK);
    assert(passport_key_from_mv(1900) == PASSPORT_KEY_NONE);
    assert(passport_key_from_mv(3300) == PASSPORT_KEY_NONE);
}

static void test_debounce(void)
{
    passport_key_debounce_t state = {0};
    assert_no_edges(step(&state, 447), PASSPORT_KEY_NONE);
    assert_no_edges(step(&state, 447), PASSPORT_KEY_NONE);
    passport_key_edges_t edges = step(&state, 447);
    assert(edges.stable == PASSPORT_KEY_OK);
    assert(edges.pressed == PASSPORT_KEY_OK);
    assert(edges.released == PASSPORT_KEY_NONE);

    assert_no_edges(step(&state, 3300), PASSPORT_KEY_OK);
    assert_no_edges(step(&state, 3300), PASSPORT_KEY_OK);
    edges = step(&state, 3300);
    assert(edges.stable == PASSPORT_KEY_NONE);
    assert(edges.pressed == PASSPORT_KEY_NONE);
    assert(edges.released == PASSPORT_KEY_OK);
}

static void test_direct_switch(void)
{
    passport_key_debounce_t state = {0};
    assert_no_edges(step(&state, 0), PASSPORT_KEY_NONE);
    assert_no_edges(step(&state, 0), PASSPORT_KEY_NONE);
    passport_key_edges_t edges = step(&state, 0);
    assert(edges.stable == PASSPORT_KEY_UP);
    assert(edges.pressed == PASSPORT_KEY_UP);

    assert_no_edges(step(&state, 150), PASSPORT_KEY_UP);
    assert_no_edges(step(&state, 150), PASSPORT_KEY_UP);
    edges = step(&state, 150);
    assert(edges.stable == PASSPORT_KEY_DOWN);
    assert(edges.pressed == PASSPORT_KEY_DOWN);
    assert(edges.released == PASSPORT_KEY_UP);
}

static void test_cw2017(void)
{
    assert(passport_cw2017_raw_to_mv(0) == 0);
    assert(passport_cw2017_raw_to_mv(4000) == 1250);
    assert(passport_cw2017_raw_to_mv(0x4000) == 0);
    assert(passport_cw2017_raw_to_mv(0xffff) == 5119);
    assert(passport_cw2017_soc_percent(0) == 0);
    assert(passport_cw2017_soc_percent(100) == 100);
    assert(passport_cw2017_soc_percent(101) == -1);
    assert(passport_cw2017_soc_percent(255) == -1);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "windows")) {
        test_windows();
    } else if (!strcmp(argv[1], "debounce")) {
        test_debounce();
    } else if (!strcmp(argv[1], "switch")) {
        test_direct_switch();
    } else if (!strcmp(argv[1], "cw2017")) {
        test_cw2017();
    } else {
        return 2;
    }
    return 0;
}
"""


class PassportBoardHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / "passport_board_harness.c"
        source.write_text(HARNESS)
        cls.binary = source.with_suffix("")
        result = subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(BOARDS),
                str(source),
                str(BOARDS / "passport_keys.c"),
                str(BOARDS / "passport_cw2017.c"),
                "-o",
                str(cls.binary),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_harness(self, test_name: str) -> None:
        result = subprocess.run([str(self.binary), test_name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_key_windows(self) -> None:
        self.run_harness("windows")

    def test_debounce_requires_three_identical_samples(self) -> None:
        self.run_harness("debounce")

    def test_key_switch_releases_then_presses_after_debounce(self) -> None:
        self.run_harness("switch")

    def test_cw2017_voltage_and_soc_readiness(self) -> None:
        self.run_harness("cw2017")


if __name__ == "__main__":
    unittest.main()
