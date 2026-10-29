/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DRIVERS_I2C_TARGET_REREGISTER_H_
#define DRIVERS_I2C_TARGET_REREGISTER_H_

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

/*
 * Register a target again after it was switched off for a transfer of our own. Switched off in
 * the middle of another master's frame, a target block may refuse until its driver is
 * initialized again (nRF TWIS): with CONFIG_DEVICE_DEINIT_SUPPORT and a driver that can be
 * deinitialized, it is.
 */
static inline int target_reregister(const struct device *dev, struct i2c_target_config *cfg)
{
	int ret = i2c_target_register(dev, cfg);

	if (ret != 0 && IS_ENABLED(CONFIG_DEVICE_DEINIT_SUPPORT)) {
		LOG_WRN("%s refuses the target (%d): initializing it again", dev->name, ret);
		ret = device_deinit(dev);
		ret = ret ? ret : device_init(dev);
		ret = ret ? ret : i2c_target_register(dev, cfg);
	}
	if (ret != 0) {
		LOG_ERR("%s: target lost: %d", dev->name, ret);
	}
	return ret;
}

#endif /* DRIVERS_I2C_TARGET_REREGISTER_H_ */
