# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Test bench of twixt on native_sim, for the twister pytest harnesses: every
# MCU is a native_sim process, and the processes with the same bus-id share one
# virtual I2C bus (twixt,native-i2c-mm). Twister builds every MCU
# (required_applications) and starts one of them as the DUT; a test starts the
# others with start_mcus(), and reads their logs or the gateway's PTYs.
#
#   sys.path.insert(0, str(<twixt> / 'scripts' / 'pylib'))
#   from twixt_bench import Mcu, Shell, start_mcus

import os
import re
import select
import subprocess
import time
import tty
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path

TIMEOUT_S = 10


def bus_users(bus_id: int) -> list[int]:
    """PIDs of the processes that have the bus `bus_id` mapped"""
    shm = f'/dev/shm/twixt_i2c_mm_{bus_id}'
    if not Path(shm).exists():
        return []  # the last process to leave removes it
    pids = []
    for maps in Path('/proc').glob('[0-9]*/maps'):
        try:
            if shm in maps.read_text():
                pids.append(int(maps.parent.name))
        except OSError:
            pass  # gone, or not ours
    return pids


class Mcu:
    """An MCU besides the DUT: its own process in its build folder, so its flash is
    the flash.bin there, and its log is mcu.log"""

    def __init__(self, build_dir: Path, args: list[str] = ()):
        self.name = build_dir.name
        self.log = build_dir / 'mcu.log'
        with self.log.open('w') as out:
            self.proc = subprocess.Popen(
                [build_dir / 'zephyr' / 'zephyr.exe', *args],
                cwd=build_dir,
                stdout=out,
                stderr=subprocess.STDOUT,
            )

    def output(self) -> str:
        return self.log.read_text(errors='replace')

    def wait_for(self, regex: str, timeout: float = TIMEOUT_S) -> re.Match:
        """The first match of `regex` in the log, waiting for it"""
        deadline = time.monotonic() + timeout
        while not (m := re.search(regex, self.output(), re.M)):
            assert time.monotonic() < deadline, f'{self.name}: no {regex!r} in\n{self.output()}'
            time.sleep(0.1)
        return m

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()


@contextmanager
def start_mcus(build_dirs: list[str], bus_id: int, args=lambda i: []) -> Iterator[list[Mcu]]:
    """Start the MCUs of `build_dirs` (sorted), the i-th with args(i); stop them after"""
    users = bus_users(bus_id)
    assert not users, f'processes left over on bus {bus_id}, kill them first: {users}'

    mcus = []
    try:
        for i, build_dir in enumerate(sorted(build_dirs)):
            mcus.append(Mcu(Path(build_dir), args(i)))
        yield mcus
    finally:
        for mcu in mcus:
            mcu.stop()


PTY = re.compile(r'pty-(\w+) connected to pseudotty: (\S+)')


def ptys(lines: list[str]) -> dict[str, str]:
    """The PTYs a gateway printed: {'a': '/dev/pts/7', ...}"""
    return dict(m.groups() for m in map(PTY.search, lines) if m)


@contextmanager
def gateway_ptys(
    dut, build_dirs: list[str], bus_id: int, boards: list[str]
) -> Iterator[dict[str, str]]:
    """Start the boards, launch the gateway (the DUT), and yield its PTY of every board"""
    with start_mcus(build_dirs, bus_id):
        dut.launch()
        lines = dut.readlines_until(regex=rf'pty-{boards[-1]} connected', timeout=TIMEOUT_S)
        paths = ptys(lines)
        assert set(paths) == set(boards), f'PTYs of the gateway: {paths}'
        yield paths


@contextmanager
def board_shells(paths: dict[str, str], boards: dict[str, int]) -> Iterator[dict[str, 'Shell']]:
    """The shell of every board, its prompt being its MCU name: mcu-a@10:~$"""
    opened = {n: Shell(paths[n], f'mcu-{n}@{addr:x}:~$ ') for n, addr in boards.items()}
    try:
        yield opened
    finally:
        for shell in opened.values():
            shell.close()


class Shell:
    """The shell of a board, on the gateway's PTY for it"""

    def __init__(self, path: str, prompt: str):
        self.prompt = prompt
        self.buf = b''
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        # A board that started before the gateway lost its first prompt
        os.write(self.fd, b'\r')
        self.read_until(prompt)

    def close(self):
        os.close(self.fd)

    def read_until(self, text: str, timeout: float = TIMEOUT_S) -> str:
        """Output up to and including `text`; what came after it is kept for the next read"""
        end = time.monotonic() + timeout
        while (i := self.buf.find(text.encode())) < 0:
            left = end - time.monotonic()
            assert left > 0, f'{self.prompt}no {text!r} in {self.buf.decode(errors="replace")!r}'
            if select.select([self.fd], [], [], left)[0]:
                self.buf += os.read(self.fd, 4096)
        i += len(text)
        out, self.buf = self.buf[:i], self.buf[i:]
        return out.decode(errors='replace')

    def run(self, command: str) -> str:
        """Output of `command`, up to the next prompt"""
        os.write(self.fd, command.encode() + b'\r')
        self.read_until(command + '\r\n')  # the echo
        return self.read_until(self.prompt)[: -len(self.prompt)]
