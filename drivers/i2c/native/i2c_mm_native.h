/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Statistics and test bench helpers of the native_sim multi-master I2C port.
 */

#ifndef DRIVERS_I2C_NATIVE_I2C_MM_NATIVE_H_
#define DRIVERS_I2C_NATIVE_I2C_MM_NATIVE_H_

#include <stdint.h>
#include <zephyr/device.h>

/**
 * @defgroup i2c_mm_native Multi-master I2C port for native_sim
 * @{
 *
 * See dts/bindings/i2c/twixt,i2c-mm-native.yaml. The test bench helpers have no
 * counterpart on hardware.
 */

/** @brief Port statistics */
struct i2c_mm_native_stats {
	/** Transfers lost to another master (-EAGAIN) */
	uint32_t arb_lost;
	/** Gave up waiting for a busy bus (-EBUSY) */
	uint32_t bus_timeouts;
	/** Address or data byte not acknowledged */
	uint32_t nacks;
	/** Target interrupts handled */
	uint32_t irqs;
	/** Stuck bus freed */
	uint32_t recoveries;
};

/**
 * @brief Copy the port statistics.
 *
 * @param dev Port.
 * @param stats Copy.
 */
void i2c_mm_native_get_stats(const struct device *dev, struct i2c_mm_native_stats *stats);

/**
 * @brief Hold SDA low, as a target reset in the middle of a read.
 *
 * The next controller recovers the bus before its START.
 *
 * @param dev Port.
 */
void i2c_mm_native_stick_bus(const struct device *dev);

/** @} */

#endif /* DRIVERS_I2C_NATIVE_I2C_MM_NATIVE_H_ */
