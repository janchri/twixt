/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TWIXT_ESP32_TARGET_H_
#define TWIXT_ESP32_TARGET_H_
#include <zephyr/device.h>

bool i2c_mm_esp32_target_matches(const struct device *dev);
/* Fails with -EBUSY while a peer's frame is in flight, unless forced: for a bus held stuck */
int i2c_mm_esp32_target_take(const struct device *dev, uint32_t idle_us, bool force);
void i2c_mm_esp32_target_give(const struct device *dev);
#endif
