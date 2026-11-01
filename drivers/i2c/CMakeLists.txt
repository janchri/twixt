# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_TWIXT_LINK OR CONFIG_I2C_MM_NATIVE OR CONFIG_I2C_MM_GPIO)
  zephyr_library()
  zephyr_library_sources(target_restore.c)
  zephyr_library_sources_ifdef(CONFIG_I2C_MM_NATIVE native/i2c_mm_native.c)
  if(CONFIG_I2C_MM_GPIO)
    zephyr_library_sources(i2c_mm_gpio.c)
    zephyr_library_include_directories(${CONFIG_I2C_MM_GPIO_PORT})
  endif()
endif()

if(CONFIG_I2C_MM_NATIVE)
  # Linux side of the shared bus (host libc), see native_simulator docs
  target_sources(native_simulator INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/native/i2c_mm_native_bottom.c)
endif()
