/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Between the Zephyr side (i2c_mm_native.c) and the Linux side (i2c_mm_native_bottom.c)
 * of the native_sim bus: the wires in shared memory. Plain C types only, both
 * sides include it.
 */

#ifndef DRIVERS_I2C_I2C_MM_NATIVE_BOTTOM_H_
#define DRIVERS_I2C_I2C_MM_NATIVE_BOTTOM_H_

#include <stdint.h>

#define I2C_MM_NATIVE_BOTTOM_KEY_MAX 9 /* address byte + first 8 data bytes */

/* i2c_mm_native_bottom_start() results */
#define I2C_MM_NATIVE_BOTTOM_OWNER 0 /* the bus is ours for now, see below */
#define I2C_MM_NATIVE_BOTTOM_LOST  1 /* lost the arbitration to another master */
#define I2C_MM_NATIVE_BOTTOM_BUSY  2 /* another master owns the bus */
#define I2C_MM_NATIVE_BOTTOM_STUCK                                                                 \
	3 /* a target holds SDA low, see i2c_mm_native_bottom_set_stuck() */

/*
 * Join the bus `bus_id`, one per process. Returns 0 or a negative errno. The
 * other functions act for this process; a `target_slot` is another process.
 */
int i2c_mm_native_bottom_attach(int bus_id);

/* Number of live processes that have a target address on the bus */
int i2c_mm_native_bottom_targets(void);

/* Test bench barrier: mark this process done; count the live ones that are */
void i2c_mm_native_bottom_set_done(void);
int i2c_mm_native_bottom_done_count(void);

/* Target address of this process, -1 for none. 0 or a negative errno. */
int i2c_mm_native_bottom_set_target(int addr);

/* Slot of the process that is a target at `addr`, this one too if `with_mine`, or -1 */
int i2c_mm_native_bottom_find_target(int addr, int with_mine);

/*
 * Controller start condition. Takes a free bus, or joins the arbitration if the
 * START of the current owner is less than `join_us` old: the lower key (address
 * byte, then data) wins. I2C_MM_NATIVE_BOTTOM_OWNER means "ours for now", a later joiner
 * may still take the bus over: call i2c_mm_native_bottom_confirm() until it is decided.
 */
int i2c_mm_native_bottom_start(const uint8_t *key, int key_len, int join_us);

/*
 * After I2C_MM_NATIVE_BOTTOM_OWNER: 1 once the bus is ours for good, 0 if another master
 * took it over, -1 while the START is still open for competitors. Decided on
 * the wall clock all processes share, not on the simulated time of one of them.
 */
int i2c_mm_native_bottom_confirm(int join_us);
void i2c_mm_native_bottom_release(void);

/*
 * 1: a target holds SDA low, as after a reset in the middle of a read, and every
 * START gets I2C_MM_NATIVE_BOTTOM_STUCK; 0: the nine clocks and STOP of a bus recovery
 * have freed it.
 */
void i2c_mm_native_bottom_set_stuck(int stuck);

/* Does any master own the bus? Cheap, for deciding how often to look at it. */
int i2c_mm_native_bottom_active(void);

/* Controller to target: post an event, then wait for the target's answer */
void i2c_mm_native_bottom_post(int target_slot, int ev, uint8_t byte);
int i2c_mm_native_bottom_answered(int target_slot, int *result, uint8_t *byte);
void i2c_mm_native_bottom_cancel(int target_slot);

/* Target side: take the pending event, if any, and answer it */
int i2c_mm_native_bottom_fetch(int *ev, uint8_t *byte);
void i2c_mm_native_bottom_answer(int result, uint8_t byte);

/* Wall clock shared by all processes */
int64_t i2c_mm_native_bottom_wall_us(void);

/* Wall clock time at which the first process of this run joined the bus */
int64_t i2c_mm_native_bottom_epoch_us(void);

#endif /* DRIVERS_I2C_I2C_MM_NATIVE_BOTTOM_H_ */
