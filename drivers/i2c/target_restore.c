/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "target_restore.h"

LOG_MODULE_REGISTER(i2c_target_restore, CONFIG_I2C_LOG_LEVEL);

static atomic_t failures;
static atomic_t down;

int target_reregister(const struct device *dev, struct i2c_target_config *cfg)
{
	int ret = i2c_target_register(dev, cfg);

	if (ret != 0 && IS_ENABLED(CONFIG_DEVICE_DEINIT_SUPPORT)) {
		LOG_WRN("%s refuses the target (%d): initializing it again", dev->name, ret);
		ret = device_deinit(dev);
		ret = ret ? ret : device_init(dev);
		ret = ret ? ret : i2c_target_register(dev, cfg);
	}
	return ret;
}

void target_restore_report(int ret)
{
	if (ret != 0) {
		LOG_ERR("target lost: %d", ret);
		atomic_inc(&failures);
	}
	atomic_set(&down, ret != 0);
}

uint32_t target_restore_failures(void)
{
	return atomic_get(&failures);
}

bool target_restore_down(void)
{
	return atomic_get(&down) != 0;
}
