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
PASSPORT_BOARD = BOARDS / "board_passport.c"
MUSE_INPUT = ROOT / "components" / "muse" / "muse_input.c"


def _split_config_if(source: str, symbol: str) -> tuple[list[str], list[str]]:
    lines = source.splitlines()
    if_bodies: list[str] = []
    else_bodies: list[str] = []
    index = 0
    needle = f"#if {symbol}"
    while index < len(lines):
        if lines[index].strip() != needle:
            index += 1
            continue
        depth = 1
        arm: list[str] = []
        other: list[str] = []
        dest = arm
        index += 1
        while index < len(lines) and depth:
            stripped = lines[index].strip()
            if stripped.startswith("#if"):
                depth += 1
                dest.append(lines[index])
            elif stripped.startswith("#endif"):
                depth -= 1
                if depth:
                    dest.append(lines[index])
            elif depth == 1 and stripped.startswith("#else"):
                dest = other
            else:
                dest.append(lines[index])
            index += 1
        if_bodies.append("\n".join(arm))
        else_bodies.append("\n".join(other))
    return if_bodies, else_bodies


def _remove_config_if(source: str, symbol: str) -> str:
    lines = source.splitlines()
    kept: list[str] = []
    index = 0
    needle = f"#if {symbol}"
    while index < len(lines):
        if lines[index].strip() != needle:
            kept.append(lines[index])
            index += 1
            continue
        depth = 1
        index += 1
        while index < len(lines) and depth:
            stripped = lines[index].strip()
            if stripped.startswith("#if"):
                depth += 1
            elif stripped.startswith("#endif"):
                depth -= 1
            index += 1
    return "\n".join(kept)

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


class PassportBoardContractTests(unittest.TestCase):
    def test_power_off_waits_for_release_then_gpio_high_before_arming_wake(self) -> None:
        source = PASSPORT_BOARD.read_text()
        power_off = source[source.index("static esp_err_t power_off(void)"):]

        self.assertIn("wait_key_released(POWER_OFF_RELEASE_MS)", power_off)
        self.assertIn("GPIO0 still low after %d ms; not arming wake", power_off)
        self.assertIn("GPIO0 high after %d ms; arming low wake", power_off)
        self.assertIn("esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown", power_off)
        self.assertLess(
            power_off.index("wait_key_released(POWER_OFF_RELEASE_MS)"),
            power_off.index("display_pause(true)"),
        )
        self.assertLess(
            power_off.index("GPIO0 still low after %d ms; not arming wake"),
            power_off.index("esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown"),
        )

    def test_passport_diagnostic_commands_remain_reachable(self) -> None:
        source = MUSE_INPUT.read_text()

        for command in ("keylevel", "i2sstat", "i2sreset", "chirp", "caption=", "snap"):
            self.assertIn(command, source)

    def test_flush_capture_defaults_off_and_is_compiled_out(self) -> None:
        kconfig = (ROOT / "components" / "muse" / "Kconfig").read_text()
        start = kconfig.index("config MUSE_PASSPORT_FLUSH_CAPTURE")
        block = kconfig[start:kconfig.index("config MUSE_CJK_FONT", start)]
        self.assertIn("depends on MUSE_BOARD_PASSPORT", block)
        self.assertIn("default n", block)

        if_bodies, else_bodies = _split_config_if(
            PASSPORT_BOARD.read_text(), "CONFIG_MUSE_PASSPORT_FLUSH_CAPTURE")
        self.assertTrue(if_bodies)
        self.assertIn("@px %ld %ld %ld %ld ", "\n".join(if_bodies))
        self.assertIn("@snap done flushes=", "\n".join(if_bodies))
        self.assertIn("@snap off", "\n".join(else_bodies))
        outside = _remove_config_if(
            PASSPORT_BOARD.read_text(), "CONFIG_MUSE_PASSPORT_FLUSH_CAPTURE")
        self.assertNotIn("@px", outside)
        self.assertNotIn("@snap done", outside)
        self.assertNotIn("@snap abort", outside)

        link_if, _link_else = _split_config_if(
            MUSE_INPUT.read_text(), "CONFIG_MUSE_PASSPORT_FLUSH_CAPTURE")
        self.assertIn('!strcmp(name, "confirm")', "\n".join(link_if))
        self.assertNotIn('!strcmp(name, "confirm")', _remove_config_if(
            MUSE_INPUT.read_text(), "CONFIG_MUSE_PASSPORT_FLUSH_CAPTURE"))
        overlay = (ROOT / "devices" / "sdkconfig.muse-passport").read_text()
        self.assertNotIn("MUSE_PASSPORT_FLUSH_CAPTURE", overlay)


if __name__ == "__main__":
    unittest.main()
