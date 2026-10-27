# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Twister builds the board and the gateway (sample.yaml) and starts the gateway;
# this test starts the board, and sends it SMP requests on the gateway's PTY.

import asyncio
import sys
from pathlib import Path

from smpclient import SMPClient
from smpclient.requests.os_management import EchoWrite, MCUMgrParametersRead
from smpclient.transport.serial import SMPSerialTransport
from twister_harness import DeviceAdapter

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts' / 'pylib'))
from twixt_bench import TIMEOUT_S, gateway_ptys  # noqa: E402

BUS_ID = 3  # boards/native_sim.overlay


async def smp(pty: str):
    async with SMPClient(SMPSerialTransport(), pty, timeout_s=TIMEOUT_S) as client:
        echo = await client.request(EchoWrite(d='hello over the bus'))
        assert echo.r == 'hello over the bus', echo
        params = await client.request(MCUMgrParametersRead())
        assert params.buf_size > 0, params


def test_smp(required_build_dirs: list[str], unlaunched_dut: DeviceAdapter):
    with gateway_ptys(unlaunched_dut, required_build_dirs, BUS_ID, ['a']) as paths:
        asyncio.run(smp(paths['a']))
