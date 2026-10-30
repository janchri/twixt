# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# The echo load of every combination of senders against every combination of
# targets: each sender runs "twixt stress start <interval> <seconds> <targets>"
# in its shell, at the same time, and the counts come from "twixt stress". The
# senders skip their own address. A cell fails on a wrong answer or a shell
# that does not answer; errors (retries exhausted) are only counted. After a
# hang the other cells are skipped. The matrix is printed at the end.

import re
import time

import pytest
from conftest import MCUS, cells, name

STATS = re.compile(r'0x([0-9a-f]{2}): ok (\d+), wrong (\d+), err (\d+)')


def test_stress(shells, load, senders, targets):
    if load['hang']:
        pytest.skip(f'no shell since {load["hang"]}')
    cell = f'{name(senders)} -> {name(targets)}'
    addrs = ' '.join(f'0x{MCUS[t]:02x}' for t in targets)

    try:
        for s in senders:
            shells[s].run(f'twixt stress start {load["interval"]} {load["seconds"]} {addrs}')
        time.sleep(load['seconds'] + 0.5)
        outs = []
        for s in senders:
            out = shells[s].run('twixt stress')
            if 'running' in out:
                shells[s].run('twixt stress stop')
                out = shells[s].run('twixt stress')
            outs.append(out)
    except AssertionError:
        cells[(senders, targets)] = 'HANG'
        load['hang'] = cell
        raise

    ok = wrong = err = 0
    for out in outs:
        for _, o, w, e in STATS.findall(out):
            ok, wrong, err = ok + int(o), wrong + int(w), err + int(e)
    cells[(senders, targets)] = f'{ok}/{wrong}/{err}'
    assert wrong == 0, f'{cell}: {wrong} wrong answers ({ok} ok, {err} errors)'
