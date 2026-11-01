# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(link.c)
zephyr_library_include_directories(. ../../drivers/i2c) # target_restore.h
zephyr_library_sources_ifdef(CONFIG_TWIXT twixt.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_PING service/ping.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_ECHO service/echo.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_SETTINGS service/settings.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_STRESS client/stress.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_SMP service/smp.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_SMP_GATEWAY client/smp_gateway.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_SMP_GATEWAY_SERIAL client/smp_gateway_serial.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_SMP_GATEWAY_BT client/smp_gateway_bt.c)
if(CONFIG_TWIXT)
  zephyr_linker_sources(SECTIONS twixt_services.ld)
endif()
