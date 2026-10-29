# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_I2C_MM OR CONFIG_I2C_MM_NATIVE OR CONFIG_I2C_MM_GPIO)
  zephyr_library()
  zephyr_library_sources_ifdef(CONFIG_I2C_MM i2c_mm.c)
  zephyr_library_sources_ifdef(CONFIG_I2C_MM_NATIVE i2c_mm_native.c)
  zephyr_library_sources_ifdef(CONFIG_I2C_MM_GPIO i2c_mm_gpio.c)
endif()

if(CONFIG_I2C_MM_NATIVE)
  # Linux side of the shared bus (host libc), see native_simulator docs
  target_sources(native_simulator INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/i2c_mm_native_bottom.c)
endif()
