/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Ping service of twixt.
 */

#ifndef TWIXT_PING_H_
#define TWIXT_PING_H_

#include <stdint.h>
#include <zephyr/kernel.h>

#include <twixt/twixt.h>

/**
 * @defgroup twixt_ping Ping service
 * @ingroup twixt
 * @{
 *
 * Does an MCU answer, and with this MCU's protocol? Enabled by default
 * (CONFIG_TWIXT_PING): every MCU should answer it.
 *
 *     request:  no data
 *     response: TWIXT_PROTOCOL_VERSION, TWIXT_MSG_MAX
 *
 * Shell: <tt>twixt ping \<addr\></tt>
 */

/** @brief What an MCU answers */
struct twixt_ping_info {
	/** Its TWIXT_PROTOCOL_VERSION */
	uint8_t version;
	/** Its TWIXT_MSG_MAX */
	uint8_t msg_max;
};

/**
 * @brief Ping an MCU.
 *
 * @param addr MCU to ping.
 * @param info What it answered.
 *
 * @retval 0 Same protocol and limits as this MCU.
 * @retval -EPROTO It answered, but differs; @p info is filled in.
 * @retval -errno See twixt_request().
 */
int twixt_ping(uint8_t addr, struct twixt_ping_info *info);

/**
 * @brief Ping an MCU until it answers, e.g. while it boots.
 *
 * @param addr MCU to ping.
 * @param info What it answered.
 * @param timeout How long to keep trying.
 *
 * @retval -EIO Nobody answered within @p timeout.
 * @retval others As twixt_ping().
 */
int twixt_ping_wait(uint8_t addr, struct twixt_ping_info *info, k_timeout_t timeout);

/** @} */

#endif /* TWIXT_PING_H_ */
