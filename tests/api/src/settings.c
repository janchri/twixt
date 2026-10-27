/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The settings service of twixt, with one MCU on the bus. Requests to the
 * MCU's own address do not go over the bus, so this covers the request handling
 * and the error codes, and the controller side only up to a NACK. Requests between MCUs need
 * several processes: tests/stress and samples/.
 *
 * Settings of the test, subtree "t":
 *   t/ro   r    4 bytes
 *   t/rw   rw   up to TWIXT_SETTINGS_VALUE_MAX bytes, stored
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/ztest.h>

#include <drivers/i2c/i2c_mm_native.h>
#include <twixt/twixt.h>
#include <twixt/settings.h>

#define ADDR       DT_REG_ADDR(TWIXT_SELF)
#define I2C_DEV    DEVICE_DT_GET(DT_BUS(TWIXT_SELF))
#define EMPTY_ADDR 0x55

static const uint8_t ro[4] = {0xde, 0xad, 0xbe, 0xef};

static struct k_spinlock lock;
static uint8_t rw[TWIXT_SETTINGS_VALUE_MAX];
static uint8_t rw_len;
static atomic_t exports;

static int t_get(const char *key, char *val, int val_len_max)
{
	int len = -ENOENT;

	K_SPINLOCK(&lock) {
		if (settings_name_steq(key, "ro", NULL)) {
			len = sizeof(ro);
			memcpy(val, ro, MIN(len, val_len_max));
		} else if (settings_name_steq(key, "rw", NULL) && rw_len > 0) {
			len = rw_len;
			memcpy(val, rw, MIN(len, val_len_max));
		}
	}
	return len;
}

static int t_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t data[TWIXT_SETTINGS_VALUE_MAX];
	ssize_t ret;

	if (settings_name_steq(key, "ro", NULL)) {
		return -EACCES;
	}
	if (!settings_name_steq(key, "rw", NULL)) {
		return -ENOENT;
	}
	if (len == 0 || len > sizeof(data)) {
		return -EINVAL;
	}
	ret = read_cb(cb_arg, data, len);
	if (ret < 0) {
		return ret;
	}
	K_SPINLOCK(&lock) {
		memcpy(rw, data, len);
		rw_len = len;
	}
	return 0;
}

static int t_export(int (*export_func)(const char *name, const void *val, size_t val_len))
{
	uint8_t data[TWIXT_SETTINGS_VALUE_MAX];
	uint8_t len;

	K_SPINLOCK(&lock) {
		len = rw_len;
		memcpy(data, rw, len);
	}
	atomic_inc(&exports);
	return len > 0 ? export_func("t/rw", data, len) : 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(t, "t", t_get, t_set, NULL, t_export);

ZTEST(twixt_settings, test_get)
{
	uint8_t val[TWIXT_SETTINGS_VALUE_MAX];

	zassert_equal(twixt_settings_get(ADDR, "t/ro", val, sizeof(val)), sizeof(ro));
	zassert_mem_equal(val, ro, sizeof(ro));
}

ZTEST(twixt_settings, test_get_short_buffer)
{
	uint8_t val[2] = {0};

	/* the whole length, but only what fits */
	zassert_equal(twixt_settings_get(ADDR, "t/ro", val, sizeof(val)), sizeof(ro));
	zassert_mem_equal(val, ro, sizeof(val));
}

ZTEST(twixt_settings, test_unknown_key)
{
	uint8_t val[TWIXT_SETTINGS_VALUE_MAX];

	zassert_equal(twixt_settings_get(ADDR, "t/nothing", val, sizeof(val)), -ENOENT);
	zassert_equal(twixt_settings_set(ADDR, "t/nothing", val, 1), -ENOENT);
	/* no handler for the subtree: settings_runtime_get() says -EINVAL */
	zassert_equal(twixt_settings_get(ADDR, "nosuch/key", val, sizeof(val)), -EINVAL);
}

/* The data of a request is op, key, NUL[, value] */
ZTEST(twixt_settings, test_bad_request)
{
	const uint8_t bad_op[] = {0x7f, 't', '/', 'r', 'o', '\0'};
	const uint8_t no_nul[] = {TWIXT_SETTINGS_OP_GET, 't', '/', 'r', 'o'};
	const uint8_t no_key[] = {TWIXT_SETTINGS_OP_GET, '\0'};
	uint8_t rsp[1];

	zassert_equal(twixt_request(ADDR, TWIXT_SERVICE_SETTINGS, bad_op, sizeof(bad_op), rsp, 1),
		      -ENOTSUP);
	zassert_equal(twixt_request(ADDR, TWIXT_SERVICE_SETTINGS, no_nul, sizeof(no_nul), rsp, 1),
		      -EINVAL);
	zassert_equal(twixt_request(ADDR, TWIXT_SERVICE_SETTINGS, no_key, sizeof(no_key), rsp, 1),
		      -EINVAL);
	zassert_equal(twixt_request(ADDR, TWIXT_SERVICE_SETTINGS, NULL, 0, rsp, 1), -EINVAL);
}

ZTEST(twixt_settings, test_set_read_only)
{
	uint8_t val = 0;

	zassert_equal(twixt_settings_set(ADDR, "t/ro", &val, 1), -EACCES);
}

ZTEST(twixt_settings, test_too_long)
{
	char key[TWIXT_SETTINGS_KEY_MAX + 1];
	uint8_t val[TWIXT_SETTINGS_VALUE_MAX + 1] = {0};

	memset(key, 'k', sizeof(key) - 1);
	key[sizeof(key) - 1] = '\0';
	zassert_equal(twixt_settings_get(ADDR, key, val, 1), -EINVAL);
	zassert_equal(twixt_settings_set(ADDR, "t/rw", val, sizeof(val)), -EINVAL);
}

/* A SET is stored by a work item: settings_load() brings the value back */
ZTEST(twixt_settings, test_set_stored)
{
	const uint8_t val[] = {1, 2, 3};
	uint8_t got[TWIXT_SETTINGS_VALUE_MAX];
	atomic_val_t before = atomic_get(&exports);

	zassert_ok(twixt_settings_set(ADDR, "t/rw", val, sizeof(val)));
	zassert_equal(twixt_settings_get(ADDR, "t/rw", got, sizeof(got)), sizeof(val));
	zassert_mem_equal(got, val, sizeof(val));

	zassert_true(WAIT_FOR(atomic_get(&exports) > before, USEC_PER_SEC, k_msleep(1)),
		     "settings not saved");

	K_SPINLOCK(&lock) {
		rw_len = 0;
	}
	zassert_ok(settings_load_subtree("t"));
	zassert_equal(twixt_settings_get(ADDR, "t/rw", got, sizeof(got)), sizeof(val));
	zassert_mem_equal(got, val, sizeof(val));
}

/* Over the bus, to an address where nobody is */
ZTEST(twixt_settings, test_nobody_there)
{
	struct i2c_mm_native_stats before, after;
	struct i2c_mm_stats bus;
	uint8_t val[TWIXT_SETTINGS_VALUE_MAX];

	i2c_mm_native_get_stats(I2C_DEV, &before);
	zassert_equal(twixt_settings_get(EMPTY_ADDR, "t/ro", val, sizeof(val)), -EIO);
	i2c_mm_native_get_stats(I2C_DEV, &after);
	zassert_equal(after.nacks, before.nacks + 1);
	twixt_get_stats(&bus);
	zassert_equal(bus.sent, 0);
}

static void *setup(void)
{
	zassert_ok(settings_subsys_init());
	return NULL;
}

ZTEST_SUITE(twixt_settings, NULL, setup, NULL, NULL, NULL);
