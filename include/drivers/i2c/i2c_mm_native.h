/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Statistics and test bench helpers of the native_sim multi-master I2C port.
 */

#ifndef DRIVERS_I2C_I2C_MM_NATIVE_H_
#define DRIVERS_I2C_I2C_MM_NATIVE_H_

#include <stdint.h>
#include <zephyr/device.h>

/**
 * @defgroup i2c_mm_native Multi-master I2C port for native_sim
 * @{
 *
 * See dts/bindings/i2c/twixt,native-i2c-mm.yaml. The test bench helpers have no
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
 * @brief Wait until targets are on the bus.
 *
 * @param dev Port.
 * @param count Targets, this one included.
 *
 * @retval 0 They are.
 * @retval -ETIMEDOUT Not within 10 s.
 */
int i2c_mm_native_wait_targets(const struct device *dev, int count);

/**
 * @brief Mark this MCU done, and wait until MCUs are.
 *
 * A barrier: once it returns, @p count MCUs, this one included, have called it.
 *
 * @param dev Port.
 * @param count MCUs to wait for.
 *
 * @retval 0 They are done.
 * @retval -ETIMEDOUT Not within 10 s.
 */
int i2c_mm_native_wait_done(const struct device *dev, int count);

/**
 * @brief Wait for the start of a round, the same moment on every MCU.
 *
 * Round 0 starts 1 s after the first MCU joined the bus, every later one
 * @p period_ms after the one before; a round that is due returns at once. Uses
 * the wall clock, so native_sim must run in real time.
 *
 * @param dev Port.
 * @param round Round, from 0.
 * @param period_ms Time between rounds.
 */
void i2c_mm_native_wait_round(const struct device *dev, uint32_t round, uint32_t period_ms);

/**
 * @brief Hold SDA low, as a target reset in the middle of a read.
 *
 * The next controller recovers the bus before its START.
 *
 * @param dev Port.
 */
void i2c_mm_native_stick_bus(const struct device *dev);

/** @} */

#endif /* DRIVERS_I2C_I2C_MM_NATIVE_H_ */
