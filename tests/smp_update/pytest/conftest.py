# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0


def pytest_addoption(parser):
    parser.addoption('--port', default=None, help="the gateway's port; without it or --ble: skipped")
    parser.addoption('--ble', default=None, help='BLE device of the MCU to update, for --port')
    parser.addoption('--ble-other', default=None, help='BLE device of another MCU on the gateway')
    parser.addoption('--v1', default=None, help='signed image 1.0.0 of the MCU')
    parser.addoption('--v2', default=None, help='signed image 2.0.0 of the MCU')
    parser.addoption('--smpmgr', default='smpmgr', help='smpmgr to run')
