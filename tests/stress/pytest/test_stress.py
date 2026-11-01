# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# The echo load of every combination of senders against every combination of
# targets: each sender runs "twixt stress start <interval> <seconds> <targets>"
# in its shell, at the same time, and the counts come from "twixt stress". The
# senders skip their own address. A cell fails on a wrong answer, on an error, on
# a shell that does not answer, or on a sender and target with no statistics or
# no answer at all. After a hang the other cells are skipped. The matrix is
# printed at the end, ok/wrong/err per cell, and each failure names its errnos.

import re
import time

import pytest
from conftest import MCUS, cells, name

STATS = re.compile(r'0x([0-9a-f]{2}): ok (\d+), wrong (\d+), err (\d+)(?: \(([^,]*))?')


def missing_traffic(senders, targets, outs):
    """The sender -> target pairs without a statistics row or without an answer"""
    missing = []
    for s, out in zip(senders, outs):
        rows = {int(a, 16): int(o) for a, o, _, _, _ in STATS.findall(out)}
        for t in targets:
            if t != s and rows.get(MCUS[t], 0) == 0:
                missing.append(f'{s} -> {t}')
    return missing


def test_shell_over_the_bus(shells):
    """Every board's shell, through its UART over the bus and the bridge"""
    for n, shell in shells.items():
        if n == 'pico':
            continue
        out = shell.run('twixt ping 0x70')
        assert '0x70: protocol 1, messages up to 64 bytes' in out, out

        out = shell.run('help')
        assert 'twixt' in out and len(out) > 512, out


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
    errors = []
    for s, out in zip(senders, outs):
        for a, o, w, e, codes in STATS.findall(out):
            ok, wrong, err = ok + int(o), wrong + int(w), err + int(e)
            print(f'{cell}: {s} -> 0x{a}: ok {o}, wrong {w}, err {e} {codes}')
            if int(e):
                errors.append(f'{s} -> 0x{a}: {e} ({codes})')
    cells[(senders, targets)] = f'{ok}/{wrong}/{err}'
    assert wrong == 0, f'{cell}: {wrong} wrong answers ({ok} ok, {err} errors)'
    assert err == 0, f'{cell}: {err} errors of {ok + err}: {"; ".join(errors)}'
    missing = missing_traffic(senders, targets, outs)
    assert not missing, f'{cell}: no traffic for {", ".join(missing)} ({ok} ok, {err} errors)'
