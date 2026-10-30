# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Twister builds a and b (sample.yaml) and starts a; this test starts b next to
# it, on the same virtual bus, and checks that b got a's MCUmgr echo.
#
# On real boards (--device-testing), b on the nRF7002 DK is the DUT and a on
# the Pico a required device, both flashed by twister; the test reads both logs.

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts' / 'pylib'))
from twixt_bench import TIMEOUT_S, real_boards, start_mcus  # noqa: E402

BUS_ID = 3  # boards/native_sim.overlay
HW_TIMEOUT_S = 15  # b repeats every 2 s
B_GETS = r'MCUmgr echo from 0x10: hello over the bus'


def test_smp(request, required_build_dirs: list[str]):
    if boards := real_boards(request):
        b, _ = boards
        b.readlines_until(regex=B_GETS, timeout=HW_TIMEOUT_S)  # a's MCUmgr answered
        return

    a = request.getfixturevalue('unlaunched_dut')
    with start_mcus(required_build_dirs, BUS_ID) as (b,):
        a.launch()
        a.readlines_until(regex=r'MCUmgr on the twixt service', timeout=TIMEOUT_S)
        b.wait_for(B_GETS)
