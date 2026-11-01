/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Multi-master I2C link: one request/response transfer each.
 */

#ifndef TWIXT_LINK_H_
#define TWIXT_LINK_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

/**
 * @defgroup twixt_link Multi-master I2C link
 * @{
 *
 * Every MCU is an I2C target at its own address and a controller when it sends.
 * A request and its response are one transfer (write, repeated START, read), so
 * no other master comes in between. The link does not interpret the bytes.
 *
 * A request runs at most once: the target forgets it once answered and at every
 * STOP. A read without a request, or after one too long for TWIXT_LINK_MSG_MAX,
 * gets the callback with an empty request.
 *
 * Needs an I2C driver that is controller and target on one block and reports a
 * lost arbitration as -EAGAIN. A target may NACK for a moment after its own
 * transfers, while its block switches back to target mode.
 */

/** Longest request and longest response, in bytes */
#define TWIXT_LINK_MSG_MAX CONFIG_TWIXT_LINK_MSG_MAX

/**
 * @brief Answer a request.
 *
 * Runs in the I2C target interrupt, or in the sending thread for a request of
 * the MCU to itself, possibly both at once: keep it short, and shared state
 * under a spinlock.
 *
 * @param req Request.
 * @param req_len Length of @p req, 0 for a read without a request.
 * @param rsp Response to write, room for TWIXT_LINK_MSG_MAX bytes.
 *
 * @return Length of the response.
 */
typedef size_t (*twixt_link_serve_cb)(const uint8_t *req, size_t req_len, uint8_t *rsp);

/** @brief Link statistics */
struct twixt_link_stats {
	/** Requests to others that got through */
	uint32_t sent;
	/** Requests of others answered */
	uint32_t served;
	/** Attempts lost to other masters, to a busy bus or to an address NACK */
	uint32_t retries;
	/** Times the target did not come back after a transfer of this MCU */
	uint32_t target_restore_failures;
	/** The target did not come back after the last transfer: others cannot reach this MCU */
	bool target_down;
};

/** @brief Link of one MCU; twixt_link_init() fills it in. */
struct twixt_link {
	/** @cond INTERNAL_HIDDEN */
	const struct device *i2c;
	twixt_link_serve_cb serve;
	struct twixt_link_stats stats;
	struct k_mutex lock;
	struct i2c_target_config target;
	uint8_t req[TWIXT_LINK_MSG_MAX];
	uint8_t req_len;
	bool req_overflow; /* in byte mode, up to the STOP */
	uint8_t rsp[TWIXT_LINK_MSG_MAX];
	uint8_t rsp_len;
	uint8_t rsp_pos;
	/** @endcond */
};

/**
 * @brief Serve requests at an address from now on.
 *
 * @param link Link.
 * @param i2c I2C bus.
 * @param addr Own target address.
 * @param serve Callback that answers the requests.
 *
 * @retval 0 Serving.
 * @retval -ENODEV Bus not ready.
 * @retval -errno Target registration failed.
 */
int twixt_link_init(struct twixt_link *link, const struct device *i2c, uint16_t addr,
		    twixt_link_serve_cb serve);

/**
 * @brief Send a request and read the response.
 *
 * Bytes the target did not answer read as 0xff. A request to the own address
 * runs the callback directly. Lost arbitration and a busy bus are retried for
 * up to CONFIG_TWIXT_LINK_RETRY_MS, an address NACK (-ENXIO from the driver) for up
 * to CONFIG_TWIXT_LINK_NACK_RETRY_MS.
 * Threads may call this concurrently.
 *
 * If reading the response fails, the request may or may not have run.
 *
 * @param link Link.
 * @param addr Target address.
 * @param req Request.
 * @param req_len Length of @p req, up to TWIXT_LINK_MSG_MAX.
 * @param rsp Response buffer.
 * @param rsp_len Bytes to read, up to TWIXT_LINK_MSG_MAX.
 *
 * @retval 0 Done.
 * @retval -ENXIO Nobody answers at @p addr: the request did not run.
 * @retval -EAGAIN Lost arbitration until the deadline: it did not run.
 * @retval -EBUSY Bus busy until the deadline: it did not run.
 * @retval -EIO The target refused a byte, as of a request too long for it, or the bus is stuck: it
 *         did not run.
 * @retval -ETIMEDOUT SCL held low for too long: it may have run.
 * @retval -EINVAL Longer than TWIXT_LINK_MSG_MAX.
 */
int twixt_link_transfer(struct twixt_link *link, uint16_t addr, const void *req, size_t req_len,
			void *rsp, size_t rsp_len);

/**
 * @brief Copy the link statistics.
 *
 * @param link Link.
 * @param stats Copy.
 */
void twixt_link_get_stats(const struct twixt_link *link, struct twixt_link_stats *stats);

/** @} */

#endif /* TWIXT_LINK_H_ */
