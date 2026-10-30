# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# On native_sim twister builds a and b (sample.yaml) and starts a; this test
# starts b next to it, on the same virtual bus. On real boards
# (--device-testing) b on the nRF7002 DK is the DUT and a on the Pico a
# required device: this test flashes both and reads both logs. Either way it
# checks the sums each one got from the other.

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts' / 'pylib'))
from twixt_bench import TIMEOUT_S, real_boards, start_mcus  # noqa: E402

BUS_ID = 6  # boards/native_sim.overlay
HW_TIMEOUT_S = 15  # the MCUs repeat every 2 s
A_GETS = r'0x11 adds 2 \+ 3 = 5'
B_GETS = r'0x10 adds 40 \+ 2 = 42'


def test_app(request, required_build_dirs: list[str]):
    if boards := real_boards(request):
        b, a = boards
        b.readlines_until(regex=B_GETS, timeout=HW_TIMEOUT_S)
        a.readlines_until(regex=A_GETS, timeout=HW_TIMEOUT_S)
        return

    a = request.getfixturevalue('unlaunched_dut')
    with start_mcus(required_build_dirs, BUS_ID) as (b,):
        a.launch()
        a.readlines_until(regex=A_GETS, timeout=TIMEOUT_S)
        b.wait_for(B_GETS)
