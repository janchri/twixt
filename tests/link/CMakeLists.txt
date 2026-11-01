# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(twixt_link_test)

target_sources(app PRIVATE src/gpio_bus.c src/link.c)
target_include_directories(app PRIVATE src)
