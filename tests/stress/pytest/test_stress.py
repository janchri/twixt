# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Three MCUs on one virtual bus, sending at the same moments: twister builds a,
# b and c (testcase.yaml) and starts a as the DUT; this test starts b and c,
# waits for the verdict of every MCU, and checks that the requests sent and
# served add up.

import re
import sys
from pathlib import Path

import pytest
from twister_harness import DeviceAdapter

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts' / 'pylib'))
from twixt_bench import start_mcus  # noqa: E402

BUS_ID = 4  # boards/native_sim.overlay
VERDICT = r'all settings verified|settings check failed'
STATS = re.compile(r'sent (\d+) requests, served (\d+) requests')
TIMEOUT_S = 30


def test_stress(required_build_dirs: list[str], unlaunched_dut: DeviceAdapter):
    # every MCU its own seed, for its own random choices
    with start_mcus(required_build_dirs, BUS_ID, lambda i: [f'--seed={2 + i}']) as peers:
        unlaunched_dut.launch()  # after the peers: all three join the bus within ~1 s anyway

        # the statistics come just before the verdict
        dut_lines = unlaunched_dut.readlines_until(regex=VERDICT, timeout=TIMEOUT_S)
        for peer in peers:
            peer.wait_for(VERDICT, timeout=TIMEOUT_S)
        outputs = {'a': '\n'.join(dut_lines)} | {p.name: p.output() for p in peers}

    for name, out in outputs.items():
        if 'all settings verified' not in out:
            pytest.fail(f'{name} failed:\n{out}')

    sent = served = 0
    for name, out in outputs.items():
        m = STATS.search(out)
        assert m, f'{name}: no statistics'
        sent += int(m[1])
        served += int(m[2])
    assert sent == served, f'{sent} requests sent, {served} served'
    assert sent > 0
