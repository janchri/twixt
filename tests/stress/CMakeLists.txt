# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# One image per MCU: nrf2 and nrf are boards with a shell, the bridge passes their shells on
# to the host. MCU picks which MCU of twixt_mcus.dtsi this image is.
# Under sysbuild (the Raspberry Pi Pico), -DMCU is in the sysbuild cache, not in this image's
if(SYSBUILD_CACHE)
  file(STRINGS ${SYSBUILD_CACHE} sysbuild_mcu REGEX "^MCU:")
  if(sysbuild_mcu)
    string(REGEX REPLACE "^MCU:[A-Z]*=" "" sysbuild_mcu "${sysbuild_mcu}")
    set(MCU ${sysbuild_mcu} CACHE STRING "" FORCE)
  endif()
  file(STRINGS ${SYSBUILD_CACHE} sysbuild_esp_peer REGEX "^stress_TWIXT_STRESS_ESP_PEER:")
  if(sysbuild_esp_peer)
    string(REGEX REPLACE "^stress_TWIXT_STRESS_ESP_PEER:[A-Z]*=" "" sysbuild_esp_peer "${sysbuild_esp_peer}")
    set(TWIXT_STRESS_ESP_PEER ${sysbuild_esp_peer} CACHE BOOL "" FORCE)
  endif()
endif()
set(MCU nrf2 CACHE STRING "MCU to build: nrf2, nrf, pico or esp")
if(MCU STREQUAL "pico")
  set(role bridge)
else()
  set(role board)
endif()
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/${role}.overlay)
option(TWIXT_STRESS_ESP_PEER "Expose the ESP32 bus shell on the Pico bridge" OFF)
if(MCU STREQUAL "esp" OR TWIXT_STRESS_ESP_PEER)
  list(APPEND DTS_EXTRA_CPPFLAGS -DTWIXT_STRESS_WITH_ESP)
endif()
if(MCU STREQUAL "pico" AND TWIXT_STRESS_ESP_PEER)
  list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/esp-bridge.overlay)
endif()
list(APPEND DTS_EXTRA_CPPFLAGS -DTWIXT_SELF_LABEL=mcu_${MCU})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(twixt_stress)

target_sources(app PRIVATE src/${role}.c)
