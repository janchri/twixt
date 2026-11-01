# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Two MCUs use each of twixt's services on the other. Twister builds them (testcase.yaml) and
# starts pico; this test starts nrf next to it, on the same virtual bus, and checks what each one
# logged. Then it starts nrf again: the greeting pico set is still in its flash.
#
# On real boards (--device-testing), nrf on the nRF7002 DK is the DUT and pico on the Pico a
# required device, both flashed by twister; the test reads both logs.

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts' / 'pylib'))
from twixt_bench import Mcu, real_boards, start_mcus  # noqa: E402

BUS_ID = 8  # boards/native_sim.overlay
TIMEOUT_S = 60  # both boot, then a round of all services
ROUND_DONE = r'MCUmgr echo from 0x'


def expected(peer: int, name: str, own: str, a: int, b: int) -> list[str]:
    return [
        f'0x{peer:02x} answers: protocol 1, messages up to 64 bytes',
        f'echo from 0x{peer:02x}: "hello from {own}"',
        f'0x{peer:02x} adds {a} + {b} = {a + b}',
        f'0x{peer:02x} is {name}',
        f'0x{peer:02x} mcu/greeting = "hi from {own}"',
        f'0x{peer:02x} mcu/name is read-only: -13',  # -EACCES
        f'MCUmgr echo from 0x{peer:02x}: hello over the bus',
    ]


PICO_GETS = [*expected(0x11, 'nrf@11', 'pico@10', 2, 3), 'protocol: 100 rounds']
NRF_GETS = expected(0x10, 'pico@10', 'nrf@11', 40, 2)


def check(output: str, lines: list[str]):
    for line in lines:
        assert line in output, f'no {line!r} in\n{output}'


def test_multi_mcu(request, required_build_dirs: list[str]):
    if boards := real_boards(request):
        nrf, pico = boards
        check('\n'.join(nrf.readlines_until(regex=ROUND_DONE, timeout=TIMEOUT_S)), NRF_GETS)
        check('\n'.join(pico.readlines_until(regex=ROUND_DONE, timeout=TIMEOUT_S)), PICO_GETS)
        return

    pico = request.getfixturevalue('unlaunched_dut')
    with start_mcus(required_build_dirs, BUS_ID) as (nrf,):
        pico.launch()
        check('\n'.join(pico.readlines_until(regex=ROUND_DONE, timeout=TIMEOUT_S)), PICO_GETS)
        nrf.wait_for(ROUND_DONE, timeout=TIMEOUT_S)
        check(nrf.output(), NRF_GETS)

        nrf.stop()
        nrf = Mcu(Path(required_build_dirs[0]))
        try:
            m = nrf.wait_for(r'greeting from flash: "(.*)"')
            assert m[1] == 'hi from pico@10', m[0]
        finally:
            nrf.stop()
