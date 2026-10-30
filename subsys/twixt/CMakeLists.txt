# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(twixt.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_PING ping.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_ECHO echo.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_SETTINGS settings.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_STRESS stress.c)
zephyr_library_sources_ifdef(CONFIG_TWIXT_SMP smp.c)
zephyr_linker_sources(SECTIONS twixt_services.ld)
