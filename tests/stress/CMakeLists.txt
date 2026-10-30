# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# One image per MCU: a and b are boards with a shell, gw is the gateway that bridges their shells
# to the host. MCU picks which MCU of twixt_mcus.dtsi this image is.
# Under sysbuild (the Raspberry Pi Pico), -DMCU is in the sysbuild cache, not in this image's
if(SYSBUILD_CACHE)
  file(STRINGS ${SYSBUILD_CACHE} sysbuild_mcu REGEX "^MCU:")
  if(sysbuild_mcu)
    string(REGEX REPLACE "^MCU:[A-Z]*=" "" sysbuild_mcu "${sysbuild_mcu}")
    set(MCU ${sysbuild_mcu} CACHE STRING "" FORCE)
  endif()
endif()
set(MCU a CACHE STRING "MCU to build: a, b or gw")
if(MCU STREQUAL "gw")
  set(role gateway)
else()
  set(role board)
endif()
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/${role}.overlay)
list(APPEND DTS_EXTRA_CPPFLAGS -DTWIXT_SELF_LABEL=mcu_${MCU})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(twixt_stress)

target_sources(app PRIVATE src/${role}.c)
