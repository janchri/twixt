# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# On native_sim twister builds the four MCUs (sample.yaml) and starts the
# gateway; this test starts the boards a, b and c, opens the PTY the gateway has
# for each, and runs shell commands there. On real boards (--device-testing)
# board b on the nRF7002 DK is the DUT and the gateway on the Pico a required
# device: the test uses b's shell on the gateway's USB port.

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts' / 'pylib'))
from twixt_bench import Shell, board_shells, gateway_ptys, pico_port, real_boards  # noqa: E402

BUS_ID = 1  # boards/native_sim.overlay
BOARDS = {'a': 0x10, 'b': 0x11, 'c': 0x12}


@pytest.fixture
def shells(request, required_build_dirs: list[str]):
    if boards := real_boards(request):
        _, gw = boards
        shell = Shell(pico_port(gw, 8), 'mcu-b@11:~$ ')  # if08: board b
        yield {'b': shell}
        shell.close()
        return

    dut = request.getfixturevalue('unlaunched_dut')
    with (
        gateway_ptys(dut, required_build_dirs, BUS_ID, list(BOARDS)) as paths,
        board_shells(paths, BOARDS) as opened,
    ):
        yield opened


def test_uart(shells: dict[str, Shell]):
    names = list(shells)
    for i, n in enumerate(names):
        shell = shells[n]
        addr = BOARDS[names[(i + 1) % len(names)]] if len(names) > 1 else 0x70

        # Over the bus twice: the command line and its output, and the ping
        out = shell.run(f'twixt ping 0x{addr:02x}')
        assert f'0x{addr:02x}: protocol 1, messages up to 64 bytes' in out, out

        # Many chunks in a row, more than the gateway's buffers
        out = shell.run('help')
        assert 'twixt' in out and len(out) > 512, out
