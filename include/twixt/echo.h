/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Echo service of twixt.
 */

#ifndef TWIXT_ECHO_H_
#define TWIXT_ECHO_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#include <twixt/twixt.h>

/**
 * @defgroup twixt_echo Echo service
 * @ingroup twixt
 * @{
 *
 * Does data come back as it was sent? The MCU answers with the request data,
 * for tests of the bus (CONFIG_TWIXT_ECHO; twixt stress, CONFIG_TWIXT_STRESS,
 * loads the bus with it).
 *
 *     request:  any data
 *     response: the same data
 *
 * Shell: <tt>twixt echo \<addr\> \<text\></tt>
 */

/** Longest data that fits a request and its response */
#define TWIXT_ECHO_MAX MIN(TWIXT_REQ_DATA_MAX, TWIXT_RSP_DATA_MAX)

/**
 * @brief Send data to an MCU and read back what it answers.
 *
 * @param addr MCU to send to.
 * @param data Data to send.
 * @param len Length of @p data, up to TWIXT_ECHO_MAX.
 * @param back Room for @p len bytes: what came back.
 *
 * @retval 0 Answered; compare @p back with @p data.
 * @retval -EINVAL Longer than TWIXT_ECHO_MAX.
 * @retval -errno See twixt_request().
 */
int twixt_echo(uint8_t addr, const void *data, size_t len, void *back);

/** @} */

#endif /* TWIXT_ECHO_H_ */
