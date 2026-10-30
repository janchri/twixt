# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# One image per MCU: MCU picks which MCU of boards/native_sim.overlay this image is
# Under sysbuild (the Raspberry Pi Pico), -DMCU is in the sysbuild cache, not in this image's
if(SYSBUILD_CACHE)
  file(STRINGS ${SYSBUILD_CACHE} sysbuild_mcu REGEX "^MCU:")
  if(sysbuild_mcu)
    string(REGEX REPLACE "^MCU:[A-Z]*=" "" sysbuild_mcu "${sysbuild_mcu}")
    set(MCU ${sysbuild_mcu} CACHE STRING "" FORCE)
  endif()
endif()
set(MCU a CACHE STRING "MCU to build: a or b")
list(APPEND DTS_EXTRA_CPPFLAGS -DTWIXT_SELF_LABEL=mcu_${MCU})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(twixt_echo)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_USB_DEVICE_STACK_NEXT app PRIVATE src/usb.c)
