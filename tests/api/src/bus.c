/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Twixt under faults, and its ping service. The native_sim port can address
 * its own target (`loopback`), so these tests send raw transfers to this MCU's target, over
 * the bus and through its interrupt: what a broken or foreign controller could
 * send, and what a controller that lost its place leaves behind. A test service
 * counts how often a request really ran; another one echoes what it got.
 */

#include <string.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include <drivers/i2c/i2c_mm_native.h>
#include <twixt/twixt.h>
#include <twixt/ping.h>

#define ADDR          DT_REG_ADDR(TWIXT_SELF)
#define I2C_DEV       DEVICE_DT_GET(DT_BUS(TWIXT_SELF))
#define EMPTY_ADDR    0x55
#define SERVICE_COUNT (TWIXT_SERVICE_APP + 0x7e)
#define SERVICE_NONE  (TWIXT_SERVICE_APP + 0x7d)
#define SERVICE_ECHO  (TWIXT_SERVICE_APP + 0x7c)
#define SERVICE_FAIL  (TWIXT_SERVICE_APP + 0x7b)
#define SERVICE_EIO   (TWIXT_SERVICE_APP + 0x7a)
#define SRC           0x42
#define BIG_ERRNO     200 /* past 127, as ENOTSUP (134) with picolibc */

static atomic_t runs;

/* The response data is how often it ran, this time included */
static int count(const struct twixt_req *req, uint8_t *rsp_data)
{
	rsp_data[0] = atomic_inc(&runs) + 1;
	return 1;
}

TWIXT_SERVICE_DEFINE(test_count_service, SERVICE_COUNT, count);

/* The response data is src, data */
static int echo(const struct twixt_req *req, uint8_t *rsp_data)
{
	rsp_data[0] = req->src;
	memcpy(&rsp_data[1], req->data, req->len);
	return 1 + req->len;
}

TWIXT_SERVICE_DEFINE(test_echo_service, SERVICE_ECHO, echo);

static int fail(const struct twixt_req *req, uint8_t *rsp_data)
{
	return -BIG_ERRNO;
}

TWIXT_SERVICE_DEFINE(test_fail_service, SERVICE_FAIL, fail);

static int eio(const struct twixt_req *req, uint8_t *rsp_data)
{
	return -EIO;
}

TWIXT_SERVICE_DEFINE(test_eio_service, SERVICE_EIO, eio);

/* Raw transfers to this MCU's own target, over the bus: the request is service, src, flags,
 * data; the response is status, data
 */
static int request(const uint8_t *req, size_t req_len, uint8_t *rsp, size_t rsp_len)
{
	return i2c_write_read(I2C_DEV, ADDR, req, req_len, rsp, rsp_len);
}

static const uint8_t count_req[] = {SERVICE_COUNT, SRC, 0};

ZTEST(twixt_faults, test_request)
{
	atomic_val_t before = atomic_get(&runs);
	uint8_t rsp[2];

	zassert_ok(request(count_req, sizeof(count_req), rsp, sizeof(rsp)));
	zassert_equal(rsp[0], 0);
	zassert_equal(rsp[1], before + 1);
	zassert_equal(atomic_get(&runs), before + 1);
}

/* A read without a request (a bus scan) gets -EBADMSG, and does not run the last request */
ZTEST(twixt_faults, test_read_alone)
{
	uint8_t rsp[2];
	atomic_val_t before;

	zassert_ok(request(count_req, sizeof(count_req), rsp, sizeof(rsp)));
	before = atomic_get(&runs);

	zassert_ok(i2c_read(I2C_DEV, rsp, sizeof(rsp), ADDR));
	zassert_equal(rsp[0], EBADMSG);
	zassert_equal(atomic_get(&runs), before, "the last request ran again");
}

/* A request whose controller gave up before the read does not run, not even later */
ZTEST(twixt_faults, test_write_alone)
{
	atomic_val_t before = atomic_get(&runs);
	uint8_t rsp[2];

	zassert_ok(i2c_write(I2C_DEV, count_req, sizeof(count_req), ADDR));
	zassert_ok(i2c_read(I2C_DEV, rsp, sizeof(rsp), ADDR));
	zassert_equal(rsp[0], EBADMSG);
	zassert_equal(atomic_get(&runs), before);
}

/*
 * The controller reads only part of the response (it was reset during the read):
 * the request ran once, and the next one gets its own fresh response
 */
ZTEST(twixt_faults, test_lost_response)
{
	atomic_val_t before = atomic_get(&runs);
	uint8_t rsp[2];

	zassert_ok(request(count_req, sizeof(count_req), rsp, 1));
	zassert_equal(atomic_get(&runs), before + 1);

	zassert_ok(request(count_req, sizeof(count_req), rsp, sizeof(rsp)));
	zassert_equal(rsp[0], 0);
	zassert_equal(rsp[1], before + 2);
}

/* A request longer than TWIXT_MSG_MAX is NACKed and not run; the next one works */
ZTEST(twixt_faults, test_too_long)
{
	uint8_t req[TWIXT_MSG_MAX + 1] = {SERVICE_COUNT, SRC, 0};
	atomic_val_t before = atomic_get(&runs);
	uint8_t rsp[2];

	zassert_equal(request(req, sizeof(req), rsp, sizeof(rsp)), -EIO);
	zassert_equal(atomic_get(&runs), before);

	zassert_ok(request(count_req, sizeof(count_req), rsp, sizeof(rsp)));
	zassert_equal(rsp[1], before + 1);
}

ZTEST(twixt_faults, test_unknown_service)
{
	const uint8_t req[] = {SERVICE_NONE, SRC, 0};
	uint8_t rsp[2];

	zassert_ok(request(req, sizeof(req), rsp, sizeof(rsp)));
	zassert_equal(rsp[0], ENOTSUP);
}

/*
 * An errno past 127 is an error, on the wire and as the result: native_sim's libc
 * has ENOTSUP at 95, Zephyr's (picolibc, minimal) at 134
 */
ZTEST(twixt_faults, test_big_errno)
{
	const uint8_t req[] = {SERVICE_FAIL, SRC, 0};
	uint8_t rsp[2];

	zassert_ok(request(req, sizeof(req), rsp, sizeof(rsp)));
	zassert_equal(rsp[0], BIG_ERRNO);
	zassert_equal(twixt_request(ADDR, SERVICE_FAIL, NULL, 0, rsp, sizeof(rsp)), -BIG_ERRNO);
}

/* A service's -EIO must not look like "nobody answers" */
ZTEST(twixt_faults, test_bus_errno)
{
	uint8_t rsp[1];

	zassert_equal(twixt_request(ADDR, SERVICE_EIO, NULL, 0, rsp, sizeof(rsp)), -EFAULT);
}

/* A request without its whole header (service, src, flags) is no request */
ZTEST(twixt_faults, test_short_header)
{
	const uint8_t req[] = {SERVICE_COUNT, SRC};
	atomic_val_t before = atomic_get(&runs);
	uint8_t rsp[2];

	zassert_ok(request(req, sizeof(req), rsp, sizeof(rsp)));
	zassert_equal(rsp[0], EBADMSG);
	zassert_equal(atomic_get(&runs), before);
}

/* The service gets src and data as they were sent, and its response data comes back whole */
ZTEST(twixt_faults, test_echo)
{
	const uint8_t req[] = {SERVICE_ECHO, SRC, 0, 1, 2, 3};
	const uint8_t want[] = {0, SRC, 1, 2, 3};
	uint8_t rsp[sizeof(want)];

	zassert_ok(request(req, sizeof(req), rsp, sizeof(rsp)));
	zassert_mem_equal(rsp, want, sizeof(want));
}

/* Flags are reserved: whatever a later version sends there, this one runs the request */
ZTEST(twixt_faults, test_flags_ignored)
{
	const uint8_t req[] = {SERVICE_ECHO, SRC, 0xff, 7};
	const uint8_t want[] = {0, SRC, 7};
	uint8_t rsp[sizeof(want)];

	zassert_ok(request(req, sizeof(req), rsp, sizeof(rsp)));
	zassert_mem_equal(rsp, want, sizeof(want));
}

/* twixt_request() puts this MCU in as src; to itself, past the response data reads as 0xff */
ZTEST(twixt_faults, test_echo_request)
{
	const uint8_t data[] = {4, 5};
	const uint8_t want[] = {ADDR, 4, 5, 0xff};
	uint8_t rsp[sizeof(want)];

	zassert_ok(twixt_request(ADDR, SERVICE_ECHO, data, sizeof(data), rsp, sizeof(rsp)));
	zassert_mem_equal(rsp, want, sizeof(want));
}

/* A target holds SDA low: the driver recovers the bus, and the request gets through */
ZTEST(twixt_faults, test_stuck_bus)
{
	struct i2c_mm_native_stats before, after;
	struct twixt_ping_info info;

	i2c_mm_native_get_stats(I2C_DEV, &before);
	i2c_mm_native_stick_bus(I2C_DEV);

	zassert_equal(twixt_ping(EMPTY_ADDR, &info), -EIO, "not through to the NACK");
	i2c_mm_native_get_stats(I2C_DEV, &after);
	zassert_equal(after.recoveries, before.recoveries + 1);
}

ZTEST_SUITE(twixt_faults, NULL, NULL, NULL, NULL, NULL);

ZTEST(twixt_ping, test_ping)
{
	struct twixt_ping_info info;

	zassert_ok(twixt_ping(ADDR, &info));
	zassert_equal(info.version, TWIXT_PROTOCOL_VERSION);
	zassert_equal(info.msg_max, TWIXT_MSG_MAX);
}

/* The same over the bus: service 0x00, src, flags; response status, version, TWIXT_MSG_MAX */
ZTEST(twixt_ping, test_ping_on_the_wire)
{
	const uint8_t req[] = {TWIXT_SERVICE_PING, SRC, 0};
	uint8_t rsp[3];

	zassert_ok(request(req, sizeof(req), rsp, sizeof(rsp)));
	zassert_equal(rsp[0], 0);
	zassert_equal(rsp[1], TWIXT_PROTOCOL_VERSION);
	zassert_equal(rsp[2], TWIXT_MSG_MAX);
}

ZTEST(twixt_ping, test_ping_nobody)
{
	struct twixt_ping_info info;

	zassert_equal(twixt_ping(EMPTY_ADDR, &info), -EIO);
}

ZTEST_SUITE(twixt_ping, NULL, NULL, NULL, NULL, NULL);
