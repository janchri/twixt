# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

# The nRF's Bluetooth controller, on its network core
if(BOARD STREQUAL "nrf7002dk")
  ExternalZephyrProject_Add(
    APPLICATION hci_ipc
    SOURCE_DIR  ${ZEPHYR_BASE}/samples/bluetooth/hci_ipc
    BOARD       nrf7002dk/nrf5340/cpunet
  )
  set(hci_ipc_EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/hci_ipc.conf CACHE INTERNAL "")
  set(hci_ipc_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/hci_ipc.overlay CACHE INTERNAL "")
  # Flashing the network core erases the application core
  sysbuild_add_dependencies(FLASH ${DEFAULT_IMAGE} hci_ipc)
  sysbuild_add_dependencies(FLASH mcuboot hci_ipc)
endif()
