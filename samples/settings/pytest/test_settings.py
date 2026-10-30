# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Twister builds a and b (sample.yaml) and starts a; this test starts b next to
# it, on the same virtual bus, and checks what each one got and set of the
# other's settings. Then it starts b again: its greeting is still in its flash.
#
# On real boards (--device-testing), b on the nRF7002 DK is the DUT and a on
# the Pico a required device, both flashed by twister; the test reads both logs.

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts' / 'pylib'))
from twixt_bench import TIMEOUT_S, Mcu, real_boards, start_mcus  # noqa: E402

BUS_ID = 0  # boards/native_sim.overlay
HW_TIMEOUT_S = 20  # the MCUs repeat every 5 s


def test_settings(request, required_build_dirs: list[str]):
    if boards := real_boards(request):
        b, a = boards
        b_out = '\n'.join(b.readlines_until(regex=r'mcu/name is read-only', timeout=HW_TIMEOUT_S))
        assert '0x10 is mcu-a@10' in b_out, b_out
        assert '0x10 mcu/greeting = "hi from mcu-b@11"' in b_out, b_out
        a_out = '\n'.join(a.readlines_until(regex=r'mcu/name is read-only', timeout=HW_TIMEOUT_S))
        assert '0x11 is mcu-b@11' in a_out, a_out
        assert '0x11 mcu/greeting = "hi from mcu-a@10"' in a_out, a_out
        return

    unlaunched_dut = request.getfixturevalue('unlaunched_dut')
    with start_mcus(required_build_dirs, BUS_ID) as (b,):
        unlaunched_dut.launch()
        lines = unlaunched_dut.readlines_until(regex=r'mcu/name is read-only', timeout=TIMEOUT_S)
        a = '\n'.join(lines)
        assert '0x11 is mcu-b@11' in a, a
        assert '0x11 mcu/greeting = "hi from mcu-a@10"' in a, a
        assert '0x11 mcu/name is read-only: -13' in a, a  # -EACCES

        b.wait_for(r'mcu/name is read-only')
        assert '0x10 is mcu-a@10' in b.output()
        assert '0x10 mcu/greeting = "hi from mcu-b@11"' in b.output()

        # a set it, and b stored it: after a restart, b has it from its flash
        b.stop()
        b = Mcu(Path(required_build_dirs[0]))
        try:
            m = b.wait_for(r'greeting from flash: "(.*)"')
            assert m[1] == 'hi from mcu-a@10', m[0]
        finally:
            b.stop()
