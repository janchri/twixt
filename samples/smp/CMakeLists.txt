# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# One image per MCU: a is the board with MCUmgr, gw the gateway to the laptop.
# MCU picks which MCU of boards/native_sim.overlay this image is.
set(MCU a CACHE STRING "MCU to build: a or gw")
if(MCU STREQUAL "gw")
  set(role gateway)
else()
  set(role board)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/board.conf)
endif()
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/${role}.overlay)
list(APPEND DTS_EXTRA_CPPFLAGS -DTWIXT_SELF_LABEL=mcu_${MCU})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(twixt_smp)

target_sources(app PRIVATE src/main.c)
