/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DRIVERS_I2C_TARGET_RESTORE_H_
#define DRIVERS_I2C_TARGET_RESTORE_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>

/*
 * Register a target again after it was switched off for a transfer of our own. Switched off in
 * the middle of another master's frame, a target block may refuse until its driver is
 * initialized again (nRF TWIS): with CONFIG_DEVICE_DEINIT_SUPPORT and a driver that can be
 * deinitialized, it is.
 */
int target_reregister(const struct device *dev, struct i2c_target_config *cfg);

/*
 * How giving the bus back to the target ended. The transfer before it stands either way: failing
 * it would invite a retry of a request that ran.
 */
void target_restore_report(int ret);

/* Failures so far, and whether the target is still down: one target block per system */
uint32_t target_restore_failures(void);
bool target_restore_down(void);

#endif /* DRIVERS_I2C_TARGET_RESTORE_H_ */
