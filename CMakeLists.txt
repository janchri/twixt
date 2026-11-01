# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(include)

add_subdirectory(drivers)
add_subdirectory(subsys)

# An image under MCUboot with MCUmgr can be flashed over MCUmgr: west flash -r smp
# (scripts/runners/smp.py), e.g. by twister on a board without a debug probe
if(CONFIG_BOOTLOADER_MCUBOOT AND CONFIG_MCUMGR)
  board_finalize_runner_args(smp)
endif()
