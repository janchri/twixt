# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Twister builds a and b (sample.yaml) and starts a; this test starts b next to
# it, on the same virtual bus, and checks that each one got its echo from the
# other.

import sys
from pathlib import Path

from twister_harness import DeviceAdapter

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts' / 'pylib'))
from twixt_bench import TIMEOUT_S, start_mcus  # noqa: E402

BUS_ID = 2  # boards/native_sim.overlay


def test_hello(required_build_dirs: list[str], unlaunched_dut: DeviceAdapter):
    with start_mcus(required_build_dirs, BUS_ID) as (b,):
        unlaunched_dut.launch()
        unlaunched_dut.readlines_until(
            regex=r'0x11 echoed "hello from mcu-a@10", from 0x10', timeout=TIMEOUT_S
        )
        b.wait_for(r'0x10 echoed "hello from mcu-b@11", from 0x11')
