/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The target side of the twixt_link link, on a fake I2C device: the test plays the driver and calls
 * the target callbacks, in byte mode and in buffer mode.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/ztest.h>

#include <twixt/link.h>

#define OWN_ADDR   0x10
#define OTHER_ADDR 0x11

static int register_ret;
static int served;
static size_t served_len;
static struct twixt_link mm;

static size_t serve(const uint8_t *req, size_t req_len, uint8_t *rsp)
{
	ARG_UNUSED(req);
	served++;
	served_len = req_len;
	rsp[0] = 0;
	return 1;
}

static int fake_configure(const struct device *dev, uint32_t config)
{
	return 0;
}

static int transfer_ret;
static int transfers;
static int busy_transfers;
static const int *script; /* the results of the first transfers, then as above */
static int script_len;

static int fake_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
			 uint16_t addr)
{
	if (++transfers <= script_len) {
		return script[transfers - 1];
	}
	return transfers <= busy_transfers ? -EBUSY : transfer_ret;
}

static int fake_target_register(const struct device *dev, struct i2c_target_config *cfg)
{
	return register_ret;
}

static DEVICE_API(i2c, fake_api) = {
	.configure = fake_configure,
	.transfer = fake_transfer,
	.target_register = fake_target_register,
};

DEVICE_DEFINE(fake_i2c, "fake_i2c", NULL, NULL, NULL, NULL, POST_KERNEL,
	      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fake_api);

#define TARGET (&mm.target)
#define CB     (mm.target.callbacks)

static void *setup(void)
{
	zassert_ok(twixt_link_init(&mm, DEVICE_GET(fake_i2c), OWN_ADDR, serve));
	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	CB->stop(TARGET);
	served = 0;
	served_len = 0xbad;
	register_ret = 0;
	transfer_ret = 0;
	transfers = 0;
	busy_transfers = 0;
	script = NULL;
	script_len = 0;
}

ZTEST(twixt_link, test_address_nack_retried)
{
	uint8_t req = 1, rsp;

	transfer_ret = -ENXIO;
	zassert_equal(twixt_link_transfer(&mm, OTHER_ADDR, &req, 1, &rsp, 1), -ENXIO);
	zassert_true(transfers > 1);
}

/* Waiting for a busy bus does not use up the time for NACKs */
ZTEST(twixt_link, test_address_nack_retried_after_busy_bus)
{
	uint8_t req = 1, rsp;

	/* backing off 2 ms or more per attempt: past the NACK time before the first NACK */
	busy_transfers = CONFIG_TWIXT_LINK_NACK_RETRY_MS;
	transfer_ret = -ENXIO;
	zassert_equal(twixt_link_transfer(&mm, OTHER_ADDR, &req, 1, &rsp, 1), -ENXIO);
	zassert_true(transfers > busy_transfers + 1);
}

/* Nor does a busy bus between two NACKs */
ZTEST(twixt_link, test_address_nack_retried_across_busy_bus)
{
	/* 1 + 4 + 6 ms of backing off: past the NACK time at the second NACK */
	static const int results[] = {-ENXIO, -EBUSY, -EBUSY, -ENXIO};
	uint8_t req = 1, rsp;

	BUILD_ASSERT(CONFIG_TWIXT_LINK_NACK_RETRY_MS < 11);
	script = results;
	script_len = ARRAY_SIZE(results);
	zassert_equal(twixt_link_transfer(&mm, OTHER_ADDR, &req, 1, &rsp, 1), 0);
	zassert_equal(transfers, ARRAY_SIZE(results) + 1);
}

ZTEST(twixt_link, test_not_retried)
{
	static const int codes[] = {-EIO, -ETIMEDOUT};
	uint8_t req = 1, rsp;

	for (size_t i = 0; i < ARRAY_SIZE(codes); i++) {
		transfer_ret = codes[i];
		transfers = 0;
		zassert_equal(twixt_link_transfer(&mm, OTHER_ADDR, &req, 1, &rsp, 1), codes[i]);
		zassert_equal(transfers, 1);
	}
}

/* A transfer that went through stands, also if the target does not come back after it */
ZTEST(twixt_link, test_target_not_restored)
{
	struct twixt_link_stats before, after;
	uint8_t req = 1, rsp;

	twixt_link_get_stats(&mm, &before);
	register_ret = -EIO;
	zassert_ok(twixt_link_transfer(&mm, OTHER_ADDR, &req, 1, &rsp, 1));
	twixt_link_get_stats(&mm, &after);
	zassert_equal(after.sent, before.sent + 1);
	zassert_equal(after.target_restore_failures, before.target_restore_failures + 1);
	zassert_true(after.target_down);

	/* back with the next transfer; the count stays */
	register_ret = 0;
	zassert_ok(twixt_link_transfer(&mm, OTHER_ADDR, &req, 1, &rsp, 1));
	twixt_link_get_stats(&mm, &after);
	zassert_equal(after.target_restore_failures, before.target_restore_failures + 1);
	zassert_false(after.target_down);
}

ZTEST(twixt_link, test_bytes_request)
{
	uint8_t val;

	CB->write_requested(TARGET);
	zassert_ok(CB->write_received(TARGET, 1));
	zassert_ok(CB->write_received(TARGET, 2));
	zassert_ok(CB->read_requested(TARGET, &val));
	zassert_equal(served, 1);
	zassert_equal(served_len, 2);
}

/* The bytes that did fit are no request, up to the STOP */
ZTEST(twixt_link, test_bytes_too_long)
{
	uint8_t val;

	CB->write_requested(TARGET);
	for (int i = 0; i < TWIXT_LINK_MSG_MAX; i++) {
		zassert_ok(CB->write_received(TARGET, 1));
	}
	zassert_equal(CB->write_received(TARGET, 1), -ENOMEM);
	zassert_equal(CB->write_received(TARGET, 1), -ENOMEM);
	zassert_ok(CB->read_requested(TARGET, &val));
	zassert_equal(served, 1);
	zassert_equal(served_len, 0);

	/* a repeated START does not make a new request of what follows */
	CB->write_requested(TARGET);
	zassert_equal(CB->write_received(TARGET, 1), -ENOMEM);
	zassert_ok(CB->read_requested(TARGET, &val));
	zassert_equal(served_len, 0);

	CB->stop(TARGET);
	CB->write_requested(TARGET);
	zassert_ok(CB->write_received(TARGET, 1));
	zassert_ok(CB->read_requested(TARGET, &val));
	zassert_equal(served_len, 1);
}

ZTEST(twixt_link, test_buffer_request)
{
	uint8_t req[TWIXT_LINK_MSG_MAX] = {0};
	uint8_t *rsp;
	uint32_t rsp_len;

	CB->buf_write_received(TARGET, req, sizeof(req));
	zassert_ok(CB->buf_read_requested(TARGET, &rsp, &rsp_len));
	zassert_equal(served, 1);
	zassert_equal(served_len, sizeof(req));
}

ZTEST(twixt_link, test_buffer_too_long)
{
	uint8_t req[TWIXT_LINK_MSG_MAX + 1] = {0};
	uint8_t *rsp;
	uint32_t rsp_len;

	CB->buf_write_received(TARGET, req, sizeof(req));
	zassert_ok(CB->buf_read_requested(TARGET, &rsp, &rsp_len));
	zassert_equal(served, 1);
	zassert_equal(served_len, 0);
}

ZTEST_SUITE(twixt_link, NULL, setup, before, NULL, NULL);

/* A STOP abandons a buffered request, but a repeated START preserves it. */
ZTEST(twixt_link, test_buffer_write_stop_read)
{
	uint8_t req[] = {1, 0x11, 0};
	uint8_t *rsp;
	uint32_t len;

	CB->buf_write_received(TARGET, req, sizeof(req));
	CB->stop(TARGET);
	zassert_ok(CB->buf_read_requested(TARGET, &rsp, &len));
	zassert_equal(served_len, 0);

	CB->buf_write_received(TARGET, req, sizeof(req));
	zassert_ok(CB->buf_read_requested(TARGET, &rsp, &len));
	zassert_equal(served_len, sizeof(req));
}
