# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Twister builds a and b (sample.yaml) and starts a; this test starts b next to
# it, on the same virtual bus, and checks that each one got the other's answer.
#
# On real boards (--device-testing), b on the nRF7002 DK is the DUT and a on
# the Pico a required device, both flashed by twister; the test reads both logs.

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts' / 'pylib'))
from twixt_bench import TIMEOUT_S, real_boards, start_mcus  # noqa: E402

BUS_ID = 2  # boards/native_sim.overlay
HW_TIMEOUT_S = 15  # the MCUs repeat every 2 s
A_GETS = r'0x11 answers: protocol 1, messages up to 64 bytes'
B_GETS = r'0x10 answers: protocol 1, messages up to 64 bytes'


def test_ping(request, required_build_dirs: list[str]):
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
