# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# An image update over the bus, with the real smpmgr, see ../README.rst. That a version runs means
# that MCUboot accepted its signature and hash after the swap.

import re
import subprocess
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts' / 'pylib'))
from twixt_bench import pico_port, real_boards  # noqa: E402

UPDATE_TIMEOUT_S = 300
BOOT_S = 20
VERSION = re.compile(r"slot=0,\s*version='([\d.]+)'.*?active=(True|False)", re.S)


def from_twister(request) -> tuple[str, str, str]:
    """On real boards with twister: pico's update port, and the two builds of the DUT"""
    try:
        boards = real_boards(request)
    except pytest.FixtureLookupError:
        pytest.skip('no --port, no --ble')
    nrf, pico = boards
    image = 'smp_update/zephyr/zephyr.signed.bin'
    v2 = request.getfixturevalue('required_build_dirs')[0]
    return pico_port(pico, 4), str(Path(nrf.device_config.build_dir) / image), str(Path(v2) / image)


@pytest.fixture
def bench(request):
    port = request.config.getoption('--port')
    ble = request.config.getoption('--ble')
    v1 = request.config.getoption('--v1')
    v2 = request.config.getoption('--v2')
    if not port and not ble:
        port, v1, v2 = from_twister(request)
    smpmgr = request.config.getoption('--smpmgr')
    other = request.config.getoption('--ble-other')
    return {
        'smpmgr': [smpmgr, '--ble', ble] if ble else [smpmgr, '--port', port],
        'other': [smpmgr, '--ble', other] if other else None,
        # A killed tool: over BLE, the link stays until CONFIG_TWIXT_SMP_GATEWAY_BT_IDLE_MS. Over
        # a port, nrf drops the packet after CONFIG_TWIXT_SMP_TIMEOUT_MS
        'after_kill': 65 if ble else 11,
        '1.0.0': v1,
        '2.0.0': v2,
    }


def active_version(bench) -> str:
    deadline = time.monotonic() + BOOT_S
    out = ''
    while time.monotonic() < deadline:
        run = subprocess.run(
            [*bench['smpmgr'], '--timeout', '10', 'image', 'state-read'],
            capture_output=True,
            text=True,
            timeout=40,
        )
        out = run.stdout + run.stderr
        match = VERSION.search(out)
        if match and match.group(2) == 'True':
            return match.group(1)
        time.sleep(1)
    pytest.fail(f'the MCU does not answer: {out[-300:]}')


def erase_slot1(bench):
    """After a swap slot 1 holds the old image: without this, going back to it transfers nothing"""
    run = subprocess.run(
        [*bench['smpmgr'], '--timeout', '60', 'image', 'erase', '1'],
        capture_output=True,
        text=True,
        timeout=90,
    )
    assert run.returncode == 0, f'erase: {(run.stdout + run.stderr)[-400:]}'


def update(bench, version):
    erase_slot1(bench)
    start = time.monotonic()
    run = subprocess.run(
        [*bench['smpmgr'], '--timeout', '90', 'upgrade', '--confirm', bench[version]],
        capture_output=True,
        text=True,
        timeout=UPDATE_TIMEOUT_S,
    )
    assert run.returncode == 0, f'upgrade to {version}: {(run.stdout + run.stderr)[-600:]}'
    time.sleep(BOOT_S)
    assert active_version(bench) == version
    print(f'{version}: {time.monotonic() - start - BOOT_S:.0f} s')


def other(version):
    return '2.0.0' if version == '1.0.0' else '1.0.0'


def test_back_and_forth(bench):
    version = active_version(bench)
    for _ in range(4):
        version = other(version)
        update(bench, version)


def test_upload_killed_halfway(bench):
    version = other(active_version(bench))
    erase_slot1(bench)
    killed = subprocess.Popen(
        [*bench['smpmgr'], '--timeout', '90', 'upgrade', bench[version]],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(6)
    killed.kill()
    killed.wait()
    time.sleep(bench['after_kill'])
    assert active_version(bench) == other(version)
    update(bench, version)


def test_other_mcu_answers_during_upload(bench):
    """The gateway has one thread, and each device its own connection"""
    if bench['other'] is None:
        pytest.skip('no --ble-other')
    version = other(active_version(bench))
    erase_slot1(bench)
    upload = subprocess.Popen(
        [*bench['smpmgr'], '--timeout', '90', 'upgrade', '--confirm', bench[version]],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(10)
    for _ in range(3):
        assert upload.poll() is None, 'the upload ended before the other MCU was asked'
        run = subprocess.run(
            [*bench['other'], '--timeout', '30', 'image', 'state-read'],
            capture_output=True,
            text=True,
            timeout=60,
        )
        out = run.stdout + run.stderr
        assert run.returncode == 0 and VERSION.search(out), f'other MCU: {out[-300:]}'
    out, _ = upload.communicate(timeout=UPDATE_TIMEOUT_S)
    assert upload.returncode == 0, f'upgrade to {version}: {out[-600:]}'
    time.sleep(BOOT_S)
    assert active_version(bench) == version
