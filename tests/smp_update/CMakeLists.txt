# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

if(SYSBUILD_CACHE)
  foreach(var MCU ROUTE VERSION)
    file(STRINGS ${SYSBUILD_CACHE} sysbuild_var REGEX "^${var}:")
    if(sysbuild_var)
      string(REGEX REPLACE "^${var}:[A-Z]*=" "" sysbuild_var "${sysbuild_var}")
      set(${var} ${sysbuild_var} CACHE STRING "" FORCE)
    endif()
  endforeach()
endif()
set(MCU pico CACHE STRING "MCU to build: pico, the gateway, or nrf, the one updated")
list(APPEND DTS_EXTRA_CPPFLAGS -DTWIXT_SELF_LABEL=mcu_${MCU})

# ROUTE=service: the twixt MCUmgr service, with twixt,smp-gateway-serial on pico. ROUTE=ble: as
# service, with nrf as the gateway, a twixt,smp-gateway-bt per MCU.
set(ROUTE service CACHE STRING "How SMP gets to the MCUs: service or ble")
if(ROUTE STREQUAL "ble")
  list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/routes/ble_${MCU}.overlay)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/routes/ble_${MCU}.conf)
elseif(MCU STREQUAL "pico")
  list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/routes/service_pico.overlay)
endif()

# VERSION: the two builds of nrf that the test updates between
if(VERSION)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/routes/version_${VERSION}.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(twixt_smp_update)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_USB_DEVICE_STACK_NEXT app PRIVATE src/usb.c)
