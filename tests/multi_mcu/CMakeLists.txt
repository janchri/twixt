# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# native_sim: MCU says which one this is. Real boards: the overlay.
set(MCU pico CACHE STRING "MCU to build on native_sim: pico or nrf")
list(APPEND DTS_EXTRA_CPPFLAGS -DTWIXT_SELF_LABEL=mcu_${MCU})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(twixt_multi_mcu)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_USB_DEVICE_STACK_NEXT app PRIVATE src/usb.c)
