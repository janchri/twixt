/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Echo load on the bus.
 */

#ifndef TWIXT_STRESS_H_
#define TWIXT_STRESS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup twixt_stress Echo load
 * @ingroup twixt
 * @{
 *
 * Does the bus carry every request right under load? A thread sends echo
 * requests round-robin to the targets, with every length up to
 * TWIXT_ECHO_MAX, and checks every answer byte (CONFIG_TWIXT_STRESS; the
 * targets need CONFIG_TWIXT_ECHO).
 *
 * Shell: <tt>twixt stress start \<interval_ms\> \<seconds\> \<addr\>...</tt>,
 * <tt>twixt stress stop</tt>, and <tt>twixt stress</tt> for the counts
 */

/** Most targets of one load */
#define TWIXT_STRESS_TARGETS_MAX 4

/** @brief The counts of one target */
struct twixt_stress_counts {
	/** Address of the target */
	uint8_t addr;
	/** Answers that came back right */
	uint32_t ok;
	/** Answers that came back with other bytes */
	uint32_t wrong;
	/** Requests that failed */
	uint32_t err;
};

/**
 * @brief Start the load.
 *
 * Clears the counts. This MCU's own address in @p addrs is skipped.
 *
 * @param interval_ms Time between the requests.
 * @param duration_ms How long; 0: until twixt_stress_stop().
 * @param addrs Targets.
 * @param n Number of @p addrs, up to TWIXT_STRESS_TARGETS_MAX.
 *
 * @retval 0 Started.
 * @retval -ENOENT No target besides this MCU: not started.
 * @retval -EBUSY Already running.
 * @retval -EINVAL Too many targets.
 */
int twixt_stress_start(uint32_t interval_ms, uint32_t duration_ms, const uint8_t *addrs, size_t n);

/** @brief Stop the load, and wait until its last request is done. */
void twixt_stress_stop(void);

/** @brief Whether the load runs. @return true while it does. */
bool twixt_stress_running(void);

/**
 * @brief Copy the counts of the last load.
 *
 * @param counts Copy, one per target.
 * @param max Room in @p counts.
 *
 * @return Number of targets copied.
 */
size_t twixt_stress_get(struct twixt_stress_counts *counts, size_t max);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* TWIXT_STRESS_H_ */
