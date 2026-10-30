# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# The shells of the MCUs for test_stress.py: on native_sim, twister starts the
# gateway and this starts the boards and opens the gateway's PTYs. On real
# boards with twister (--device-testing), board b on the nRF7002 DK and the
# gateway on the Pico, the shells are on the gateway's USB ports; without
# twister, --ports names the serial port of each MCU's shell. The matrix has
# only the MCUs there are.

import itertools
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts' / 'pylib'))
from twixt_bench import Shell, gateway_ptys, pico_port, real_boards  # noqa: E402

BUS_ID = 4  # boards/native_sim.overlay
MCUS = {'a': 0x10, 'b': 0x11, 'gw': 0x70}  # twixt_mcus.dtsi

cells = {}


def pytest_addoption(parser):
    parser.addoption(
        '--ports',
        default=None,
        help='real boards: <mcu>=<port>,..., e.g. gw=/dev/ttyACM2,b=/dev/ttyACM1',
    )
    parser.addoption('--interval', type=int, default=5, help='ms between echo requests (5)')
    parser.addoption('--seconds', type=int, default=1, help='seconds per cell (1)')
    parser.addoption('--senders', default=None, help='only these rows: <combo>,..., e.g. a+b+gw')


def ports(config) -> dict[str, str]:
    opt = config.getoption('--ports')
    return dict(p.split('=', 1) for p in opt.split(',')) if opt else {}


def mcus(config) -> list[str]:
    harness = getattr(config, 'twister_harness_config', None)
    if harness and harness.devices and harness.devices[0].type == 'hardware':
        return ['b', 'gw']  # testcase.yaml: twixt.stress.nrf7002dk
    return list(ports(config)) or list(MCUS)


def combos(names: list[str]) -> list[tuple[str, ...]]:
    """Every combination of the MCUs, by size, in the order of MCUS"""
    names = [n for n in MCUS if n in names]
    return [c for k in range(1, len(names) + 1) for c in itertools.combinations(names, k)]


def name(combo: tuple[str, ...]) -> str:
    return '+'.join(combo)


def pytest_generate_tests(metafunc):
    if 'senders' in metafunc.fixturenames:
        cs = combos(mcus(metafunc.config))
        rows = metafunc.config.getoption('--senders')
        rs = [c for c in cs if not rows or name(c) in rows.split(',')]
        pairs = list(itertools.product(rs, cs))
        metafunc.parametrize(
            'senders,targets', pairs, ids=[f'{name(s)}-to-{name(t)}' for s, t in pairs]
        )


def prompt(n: str) -> str:
    return f'{"gw" if n == "gw" else "mcu-" + n}@{MCUS[n]:x}:~$ '


@pytest.fixture(scope='session')
def shells(request):
    """{mcu: Shell} of every MCU in the test"""
    hw = ports(request.config)
    if hw:
        opened = {n: Shell(p, prompt(n)) for n, p in hw.items()}
        yield opened
        for s in opened.values():
            s.close()
        return

    if boards := real_boards(request):
        _, gw = boards
        # if04 the gateway's shell, if08 board b's
        opened = {n: Shell(pico_port(gw, i), prompt(n)) for n, i in (('gw', 4), ('b', 8))}
        yield opened
        for s in opened.values():
            s.close()
        return

    dut = request.getfixturevalue('unlaunched_dut')
    build_dirs = request.getfixturevalue('required_build_dirs')
    with gateway_ptys(dut, build_dirs, BUS_ID, list(MCUS)) as paths:
        opened = {n: Shell(paths[n], prompt(n)) for n in MCUS}
        yield opened
        for s in opened.values():
            s.close()


@pytest.fixture(scope='session')
def load(request):
    return {
        'interval': request.config.getoption('--interval'),
        'seconds': request.config.getoption('--seconds'),
        'hang': None,
    }


def pytest_terminal_summary(terminalreporter):
    if not cells:
        return

    def order(combos):
        return sorted(combos, key=lambda c: (len(c), [list(MCUS).index(n) for n in c]))

    rows = order({s for s, _ in cells})
    cols = order({t for _, t in cells})
    w = max(*(len(name(c)) for c in cols), *(len(v) for v in cells.values()))
    tr = terminalreporter
    tr.section('twixt stress: ok/wrong/err, rows send, columns are the targets')
    tr.write_line('senders \\ targets'.ljust(18) + ''.join(name(t).rjust(w + 2) for t in cols))
    for s in rows:
        tr.write_line(name(s).ljust(18) + ''.join(cells.get((s, t), '').rjust(w + 2) for t in cols))
