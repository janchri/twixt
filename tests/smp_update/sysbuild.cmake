# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

if(SB_CONFIG_NET_CORE_IMAGE_HCI_IPC)
  ExternalZephyrProject_Add(
    APPLICATION hci_ipc
    SOURCE_DIR  ${ZEPHYR_BASE}/samples/bluetooth/hci_ipc
    BOARD       nrf7002dk/nrf5340/cpunet
  )
  set(hci_ipc_EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/routes/hci_ipc.conf CACHE INTERNAL "")
  set(hci_ipc_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/routes/hci_ipc.overlay CACHE INTERNAL "")
  # Flashing the network core erases the application core
  sysbuild_add_dependencies(FLASH ${DEFAULT_IMAGE} hci_ipc)
  sysbuild_add_dependencies(FLASH mcuboot hci_ipc)
endif()
