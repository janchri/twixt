# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Twister builds the four MCUs (sample.yaml) and starts the gateway; this test
# starts the boards a, b and c, opens the PTY the gateway has for each, and runs
# shell commands there.

import sys
from pathlib import Path

import pytest
from twister_harness import DeviceAdapter

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts' / 'pylib'))
from twixt_bench import Shell, board_shells, gateway_ptys  # noqa: E402

BUS_ID = 1  # boards/native_sim.overlay
BOARDS = {'a': 0x10, 'b': 0x11, 'c': 0x12}


@pytest.fixture
def shells(required_build_dirs: list[str], unlaunched_dut: DeviceAdapter):
    with (
        gateway_ptys(unlaunched_dut, required_build_dirs, BUS_ID, list(BOARDS)) as paths,
        board_shells(paths, BOARDS) as opened,
    ):
        yield opened


def test_uart(shells: dict[str, Shell]):
    names = list(BOARDS)
    for i, n in enumerate(names):
        shell = shells[n]
        addr = BOARDS[names[(i + 1) % len(names)]]

        # Over the bus twice: the command line and its output, and the ping
        out = shell.run(f'twixt ping 0x{addr:02x}')
        assert f'0x{addr:02x}: protocol 1, messages up to 64 bytes' in out, out

        # Many chunks in a row, more than the gateway's buffers
        out = shell.run('help')
        assert 'twixt' in out and len(out) > 512, out
