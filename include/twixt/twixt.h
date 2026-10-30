/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief twixt: services between equal MCUs on a multi-master I2C bus.
 */

#ifndef TWIXT_TWIXT_H_
#define TWIXT_TWIXT_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/iterable_sections.h>

#include <drivers/i2c/i2c_mm.h>

/**
 * @defgroup twixt twixt
 * @{
 *
 * This MCU is /chosen twixt,self, a "twixt,mcu" on its I2C bus.
 * It serves requests at its address from boot on and sends requests to the
 * others, one link transfer each (@ref i2c_mm):
 *
 *     request:   service, src, flags, data
 *     response:  status, data
 *
 * @c service picks the service (see the service ids), @c src is the sender's
 * address, @c flags is reserved: written as 0, ignored on receipt. @c status is 0 or an errno
 * without its sign; an error has no data. EBADMSG means the request did not arrive whole
 * (shorter than its header): no service ran, and twixt_request() sends it again.
 *
 * The lowest frame wins arbitration, so a lower service id has the higher
 * priority.
 */

/**
 * @name Service ids
 *
 * The same on every MCU of the bus. Ids below TWIXT_SERVICE_APP belong to
 * twixt; applications use TWIXT_SERVICE_APP and up.
 * @{
 */
#define TWIXT_SERVICE_PING     0x00 /**< <twixt/ping.h> */
#define TWIXT_SERVICE_ECHO     0x01 /**< <twixt/echo.h> */
#define TWIXT_SERVICE_SETTINGS 0x02 /**< <twixt/settings.h> */
#define TWIXT_SERVICE_UART     0x10 /**< twixt,uart */
#define TWIXT_SERVICE_SMP      0x20 /**< <twixt/smp.h> */
#define TWIXT_SERVICE_APP      0x80 /**< First id for applications */
/** @} */

/** Protocol version; every MCU on a bus needs the same */
#define TWIXT_PROTOCOL_VERSION 1

/** Longest request and longest response, header and status included */
#define TWIXT_MSG_MAX I2C_MM_MSG_MAX

/** Request header: service, src, flags */
#define TWIXT_REQ_HDR_LEN 3

/** Longest data of a request */
#define TWIXT_REQ_DATA_MAX (TWIXT_MSG_MAX - TWIXT_REQ_HDR_LEN)

/** Longest data of a response */
#define TWIXT_RSP_DATA_MAX (TWIXT_MSG_MAX - 1)

/** Devicetree node of this MCU */
#define TWIXT_SELF DT_CHOSEN(twixt_self)

/** @brief A request as a service gets it */
struct twixt_req {
	/** Address of the sender */
	uint8_t src;
	/** Data after the header */
	const uint8_t *data;
	/** Length of @p data */
	size_t len;
};

/** @brief A service */
struct twixt_service {
	/** Service id */
	uint8_t id;
	/**
	 * @brief Answer a request.
	 *
	 * Runs as i2c_mm_serve_cb does. -EIO, -EAGAIN and -EBUSY mean "bus" to the
	 * caller, so twixt sends them as -EFAULT.
	 *
	 * @param req Request.
	 * @param rsp_data Response data to write, room for TWIXT_RSP_DATA_MAX bytes.
	 *
	 * @return Length of the response data, or -errno.
	 */
	int (*handler)(const struct twixt_req *req, uint8_t *rsp_data);
};

/**
 * @brief Define a service.
 *
 * @param name Name of the definition.
 * @param _id Service id; unique on the MCU, else twixt does not start.
 * @param _handler Handler, see twixt_service::handler.
 */
#define TWIXT_SERVICE_DEFINE(name, _id, _handler)                                                  \
	static const STRUCT_SECTION_ITERABLE(twixt_service, name) = {                              \
		.id = _id,                                                                         \
		.handler = _handler,                                                               \
	}

/** @brief Name of this MCU. @return Its devicetree node name. */
const char *twixt_self_name(void);

/** @brief I2C address of this MCU. @return Address. */
uint8_t twixt_self_addr(void);

/** @brief Copy the statistics of this MCU's link. @param stats Copy. */
void twixt_get_stats(struct i2c_mm_stats *stats);

/**
 * @brief Send a request and read the data of the response.
 *
 * Transfers as i2c_mm_transfer() does; data the service did not write reads
 * as 0xff.
 *
 * @param addr MCU to send to, may be this one.
 * @param service Service id.
 * @param data Data of the request.
 * @param len Length of @p data, up to TWIXT_REQ_DATA_MAX.
 * @param rsp_data Response data buffer.
 * @param rsp_len Response data bytes to read, up to TWIXT_RSP_DATA_MAX.
 *
 * @retval 0 Done.
 * @retval -EIO Nobody answers at @p addr.
 * @retval -EAGAIN Lost arbitration until the deadline.
 * @retval -EBUSY Bus busy until the deadline.
 * @retval -ENOTSUP No such service there.
 * @retval -EBADMSG The request did not arrive whole, three times.
 * @retval -errno Error of the service.
 */
int twixt_request(uint8_t addr, uint8_t service, const void *data, size_t len, void *rsp_data,
		  size_t rsp_len);

#if defined(CONFIG_TWIXT_SHELL) || defined(__DOXYGEN__)
struct shell;

/**
 * @brief Parse an I2C address, for shell commands of services.
 *
 * Services add their commands under "twixt" with SHELL_SUBCMD_ADD((twixt), ...).
 *
 * @param sh Shell, for the error.
 * @param arg Text to parse.
 * @param addr Address.
 *
 * @retval 0 Parsed.
 * @retval -EINVAL Not an address; the error is printed.
 */
int twixt_shell_parse_addr(const struct shell *sh, const char *arg, uint8_t *addr);
#endif

/** @} */

#endif /* TWIXT_TWIXT_H_ */
