# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

"""Runner that updates an image over MCUmgr (SMP), with smpmgr.

For an MCU that runs under MCUboot with MCUmgr on a serial port, e.g. a USB
CDC-ACM port: the signed image goes to the second slot, is confirmed, and
the MCU resets into it. MCUboot itself must already be on the MCU.

Several --port may be given, also as glob patterns, such as
/dev/serial/by-id/usb-twixt_*-if02: the first one that exists and answers is
used, as the port of MCUmgr can differ from image to image. After the reset
the runner waits until one of them is back, as the MCU enumerates again.
"""

import glob
import shutil
import subprocess
import time
from pathlib import Path

from runners.core import RunnerCaps, ZephyrBinaryRunner


class SmpBinaryRunner(ZephyrBinaryRunner):
    """Runner front-end for smpmgr"""

    def __init__(self, cfg, ports, timeout, wait, smpmgr):
        super().__init__(cfg)
        self.ports = ports
        self.timeout = timeout
        self.wait = wait
        self.smpmgr = smpmgr

    @classmethod
    def name(cls):
        return 'smp'

    @classmethod
    def capabilities(cls):
        return RunnerCaps(commands={'flash'}, file=True)

    @classmethod
    def do_add_parser(cls, parser):
        parser.add_argument(
            '--port',
            dest='ports',
            action='append',
            required=True,
            help='serial port of MCUmgr; may be given more than once',
        )
        parser.add_argument(
            '--timeout',
            type=float,
            default=60,
            help='seconds smpmgr waits for an answer (60: the slot erase)',
        )
        parser.add_argument(
            '--wait',
            type=float,
            default=30,
            help='seconds to wait for the port after the reset (30)',
        )
        parser.add_argument('--smpmgr', default='smpmgr', help='smpmgr to run')

    @classmethod
    def do_create(cls, cfg, args):
        return SmpBinaryRunner(cfg, args.ports, args.timeout, args.wait, args.smpmgr)

    def image(self) -> str:
        image = self.cfg.file or self.cfg.bin_file
        if not image or not Path(image).exists():
            raise RuntimeError(f'no image to send: {image}')
        if not self.cfg.file and 'signed' not in Path(image).name:
            raise RuntimeError(f'{image} is not signed: build with MCUboot')
        return image

    def do_run(self, command, **kwargs):
        if shutil.which(self.smpmgr) is None:
            raise RuntimeError(f'{self.smpmgr} not found: pip install smpmgr')
        image = self.image()

        tried = []
        for port in self.existing_ports():
            tried.append(port)
            self.logger.info(f'{image} over MCUmgr on {port}')
            ret = subprocess.run(
                [
                    self.smpmgr,
                    '--port',
                    port,
                    '--timeout',
                    str(self.timeout),
                    'upgrade',
                    '--confirm',
                    image,
                ]
            )
            if ret.returncode == 0:
                self.wait_for_port()
                return
        raise RuntimeError(f'no MCUmgr answered on {tried or "no port: " + str(self.ports)}')

    def existing_ports(self) -> list[str]:
        return [p for pattern in self.ports for p in sorted(glob.glob(pattern))]

    def wait_for_port(self):
        """The reset takes the ports away, MCUboot swaps, the image enumerates again"""
        end = time.monotonic() + self.wait
        while self.existing_ports():
            if time.monotonic() > end:
                self.logger.warning(f'{self.ports} did not go away: no reset?')
                return
            time.sleep(0.1)
        while not self.existing_ports():
            if time.monotonic() > end:
                raise RuntimeError(f'none of {self.ports} back within {self.wait} s')
            time.sleep(0.2)
        time.sleep(1)  # let the other ports of the device come too
