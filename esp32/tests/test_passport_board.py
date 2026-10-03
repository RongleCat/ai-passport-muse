# SPDX-License-Identifier: Apache-2.0

"""Host-test contract for the FoloToy AI Passport board helpers.

This file intentionally stays skipped until T1 publishes the concrete public
function names and signatures in `orchestration/results/T1.md`, together with
`passport_keys.h` and `passport_cw2017.h`. The eventual C harness must include
only those pure-helper headers and assert these hardware contracts:

* ADC millivolts classify as UP `[0, 150)`, DOWN `[150, 447)`, and OK
  `[447, 1900)`; the release reading around 3300 mV is no key.
* Debouncing emits an edge only after the implementation's documented stable
  sample count/time, and must not combine two different ADC windows.
* CW2017 voltage is `(raw & 0x3fff) * 312.5 uV`; SOC values over 100 are not
  ready.

The optional rounded-row-span harness is deliberately deferred too: it must
compile the production helper Grok places in a host-compilable source file,
not a second implementation copied from the reference BSP.
"""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
KEYS_HEADER = ROOT / "components" / "muse" / "boards" / "passport_keys.h"
CW2017_HEADER = ROOT / "components" / "muse" / "boards" / "passport_cw2017.h"


@unittest.skipUnless(
    KEYS_HEADER.is_file() and CW2017_HEADER.is_file(),
    "waiting for T1 Passport pure-helper headers and published API contract",
)
class PassportBoardHostTestContract(unittest.TestCase):
    def test_adc_windows_and_debounce(self) -> None:
        self.fail("Replace this skeleton with the T1-published keys harness.")

    def test_cw2017_voltage_and_soc_readiness(self) -> None:
        self.fail("Replace this skeleton with the T1-published CW2017 harness.")

    def test_rounded_row_spans_when_available(self) -> None:
        self.fail("Add only when T1 exports the production rounding helper for host compilation.")


if __name__ == "__main__":
    unittest.main()
