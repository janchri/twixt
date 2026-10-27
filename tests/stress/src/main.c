/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stress test of twixt: one MCU of three on a bus of equal peers.
 *
 * This image is one MCU. Its settings (Zephyr settings subsystem, subtree
 * "mcu") are read-only (r) or read/write (rw), and all MCUs get and set them by
 * name over the bus (twixt/settings.h):
 *
 *   mcu/name       r   the devicetree node of the MCU
 *   mcu/version    r   APP_VERSION
 *   mcu/counter    r   the rounds of this MCU so far
 *   mcu/dump0..2   rw  any bytes, up to TWIXT_SETTINGS_VALUE_MAX; kept in flash
 *
 * First every MCU pings the others: same protocol version, same limits. Then
 * every round, all MCUs at the very same moment send a random request to a
 * random MCU, maybe themselves, so the requests collide on the bus and the
 * lowest frame wins the arbitration; a little later they send another one. They
 * check the answer:
 *  - name and version are what the devicetree and the build say,
 *  - a counter never goes back, and cannot be set,
 *  - a dump is whole: whoever set it put a CRC into its last byte,
 *  - a setting that does not exist is not there.
 *
 * The MCUs on the bus and their I2C addresses are devicetree
 * (boards/native_sim.overlay, compatible "twixt,mcu"); the one this
 * image runs on is /chosen twixt,self, picked by -DMCU=<a|b|c>.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>

#include <drivers/i2c/i2c_mm_native.h>
#include <twixt/twixt.h>
#include <twixt/ping.h>
#include <twixt/settings.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define I2C_DEV DEVICE_DT_GET(DT_BUS(TWIXT_SELF))

#define APP_VERSION "1.0.0"
#define DUMPS       3
#define ROUNDS      20
#define PERIOD_MS   100
#define PROBE_ADDR  0x55 /* nobody there */

#define MCU(node)  DT_REG_ADDR(node),
#define NAME(node) DT_NODE_FULL_NAME(node),

static const uint8_t mcus[] = {DT_FOREACH_STATUS_OKAY(twixt_mcu, MCU)};
static const char *const names[] = {DT_FOREACH_STATUS_OKAY(twixt_mcu, NAME)};

#define NUM_MCUS ARRAY_SIZE(mcus)

BUILD_ASSERT(NUM_MCUS > 1, "no other MCU on the bus");

static uint32_t bad; /* checks that failed */

/*
 * The handlers run in the I2C target interrupt for the other MCUs, and in
 * threads for this one (settings_load(), settings_save(), any local use of the
 * settings API): `lock` keeps every value whole.
 */
static struct k_spinlock lock;
static uint32_t counter;
static struct {
	uint8_t len; /* 0: not set */
	uint8_t data[TWIXT_SETTINGS_VALUE_MAX];
} dumps[DUMPS];

/* "dump<i>" to i, or -1 */
static int dump_index(const char *key)
{
	if (strncmp(key, "dump", 4) == 0 && key[4] >= '0' && key[4] < '0' + DUMPS &&
	    key[5] == '\0') {
		return key[4] - '0';
	}
	return -1;
}

static int mcu_get(const char *key, char *val, int val_len_max)
{
	int i = dump_index(key);
	const void *src = NULL;
	int len = -ENOENT;

	if (settings_name_steq(key, "name", NULL)) {
		src = twixt_self_name();
		len = strlen(twixt_self_name());
	} else if (settings_name_steq(key, "version", NULL)) {
		src = APP_VERSION;
		len = strlen(APP_VERSION);
	} else if (settings_name_steq(key, "counter", NULL)) {
		src = &counter;
		len = sizeof(counter);
	}

	K_SPINLOCK(&lock) {
		if (i >= 0) {
			src = dumps[i].data;
			len = dumps[i].len > 0 ? dumps[i].len : -ENOENT;
		}
		if (len > val_len_max) {
			len = -ENOMEM;
		} else if (len > 0) {
			memcpy(val, src, len);
		}
	}
	return len;
}

/* Called by settings_load() with the stored dumps, and for a SET */
static int mcu_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t data[TWIXT_SETTINGS_VALUE_MAX];
	int i = dump_index(key);
	ssize_t ret;

	if (settings_name_steq(key, "name", NULL) || settings_name_steq(key, "version", NULL) ||
	    settings_name_steq(key, "counter", NULL)) {
		return -EACCES;
	}
	if (i < 0) {
		return -ENOENT;
	}
	if (len == 0 || len > sizeof(dumps[i].data)) {
		return -EINVAL;
	}
	ret = read_cb(cb_arg, data, len); /* not under the lock: may read flash */
	if (ret < 0) {
		return ret;
	}
	K_SPINLOCK(&lock) {
		memcpy(dumps[i].data, data, len);
		dumps[i].len = len;
	}
	return 0;
}

/* Called by settings_save(): the rw settings go to flash */
static int mcu_export(int (*export_func)(const char *name, const void *val, size_t val_len))
{
	for (int i = 0; i < DUMPS; i++) {
		uint8_t data[TWIXT_SETTINGS_VALUE_MAX];
		char name[sizeof("mcu/dump0")];
		uint8_t len;

		K_SPINLOCK(&lock) { /* not export_func() under the lock: it writes flash */
			len = dumps[i].len;
			memcpy(data, dumps[i].data, len);
		}

		if (len > 0) {
			snprintf(name, sizeof(name), "mcu/dump%c", '0' + i);
			export_func(name, data, len);
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(mcu, "mcu", mcu_get, mcu_set, NULL, mcu_export);

/*
 * Random number in 0..n-1. Not `% n`: the host random() behind the native_sim
 * entropy driver has a weak lowest bit, and with consecutive seeds all MCUs
 * would draw the same sequence. Bits 8..23 are fine.
 */
static uint32_t rand_below(uint32_t n)
{
	return (((sys_rand32_get() >> 8) & 0xffff) * n) >> 16;
}

static uint8_t dump_crc(const uint8_t *data, int len)
{
	return crc8_ccitt(0x00, data, len - 1);
}

static void check(uint8_t round, uint8_t addr, const char *what, bool ok, int ret)
{
	if (ok) {
		LOG_INF("round %u: 0x%02x %s: %d", round, addr, what, ret);
	} else {
		LOG_ERR("round %u: 0x%02x %s: unexpected %d", round, addr, what, ret);
		bad++;
	}
}

static void random_request(uint8_t round)
{
	static uint32_t last_counter[NUM_MCUS];
	int t = rand_below(NUM_MCUS);
	uint8_t addr = mcus[t];
	uint8_t buf[TWIXT_SETTINGS_VALUE_MAX + 1] = {0}; /* room for a NUL */
	char dump[sizeof("mcu/dump0")];
	char what[sizeof("get mcu/dump0")];
	uint32_t value = 0;
	int len;
	int ret;

	snprintf(dump, sizeof(dump), "mcu/dump%c", '0' + (int)rand_below(DUMPS));

	switch (rand_below(7)) {
	case 0:
		ret = twixt_settings_get(addr, "mcu/name", buf, TWIXT_SETTINGS_VALUE_MAX);
		check(round, addr, "get mcu/name", ret > 0 && strcmp(buf, names[t]) == 0, ret);
		break;
	case 1:
		ret = twixt_settings_get(addr, "mcu/version", buf, TWIXT_SETTINGS_VALUE_MAX);
		check(round, addr, "get mcu/version", ret > 0 && strcmp(buf, APP_VERSION) == 0,
		      ret);
		break;
	case 2:
		ret = twixt_settings_get(addr, "mcu/counter", &value, sizeof(value));
		check(round, addr, "get mcu/counter",
		      ret == sizeof(value) && value >= last_counter[t], ret);
		last_counter[t] = MAX(last_counter[t], value);
		break;
	case 3:
		ret = twixt_settings_set(addr, "mcu/counter", &value, sizeof(value));
		check(round, addr, "set mcu/counter (read-only)", ret == -EACCES, ret);
		break;
	case 4:
		len = 2 + rand_below(TWIXT_SETTINGS_VALUE_MAX - 1);
		sys_rand_get(buf, len - 1);
		buf[len - 1] = dump_crc(buf, len);
		ret = twixt_settings_set(addr, dump, buf, len);
		snprintf(what, sizeof(what), "set %s", dump);
		check(round, addr, what, ret == 0, ret);
		break;
	case 5:
		ret = twixt_settings_get(addr, dump, buf, TWIXT_SETTINGS_VALUE_MAX);
		snprintf(what, sizeof(what), "get %s", dump); /* not set, or whole */
		check(round, addr, what,
		      ret == -ENOENT || (ret >= 2 && buf[ret - 1] == dump_crc(buf, ret)), ret);
		break;
	default:
		ret = twixt_settings_get(addr, "mcu/nothing", buf, TWIXT_SETTINGS_VALUE_MAX);
		check(round, addr, "get mcu/nothing", ret == -ENOENT, ret);
		break;
	}
}

int main(void)
{
	struct i2c_mm_native_stats s;
	struct i2c_mm_stats bus;
	uint8_t byte;
	int ret;

	ret = settings_subsys_init();
	if (ret == 0) {
		ret = settings_load();
	}
	if (ret) {
		LOG_ERR("%s: cannot load the settings: %d", twixt_self_name(), ret);
		return ret;
	}
	LOG_INF("%s at 0x%02x, version %s; dumps from flash: %u, %u, %u bytes", twixt_self_name(),
		twixt_self_addr(), APP_VERSION, dumps[0].len, dumps[1].len, dumps[2].len);

	/* Test bench: the other MCUs are processes that start at their own pace */
	ret = i2c_mm_native_wait_targets(I2C_DEV, NUM_MCUS);
	if (ret) {
		LOG_ERR("%s: the other MCUs did not show up", twixt_self_name());
		return ret;
	}

	for (int i = 0; i < NUM_MCUS; i++) {
		struct twixt_ping_info info;

		ret = twixt_ping(mcus[i], &info);
		check(0, mcus[i], "ping", ret == 0, ret);
	}

	ret = twixt_settings_get(PROBE_ADDR, "mcu/name", &byte, 1);
	check(0, PROBE_ADDR, "get mcu/name (nobody there)", ret == -EIO, ret);

	for (uint8_t round = 0; round < ROUNDS; round++) {
		i2c_mm_native_wait_round(I2C_DEV, round, PERIOD_MS);
		K_SPINLOCK(&lock) {
			counter++;
		}
		random_request(round);
		k_msleep(rand_below(PERIOD_MS / 2));
		random_request(round);
	}

	/* After that nobody sends any more: the counts are final */
	ret = i2c_mm_native_wait_done(I2C_DEV, NUM_MCUS);
	if (ret) {
		LOG_ERR("%s: the other MCUs did not finish", twixt_self_name());
		return ret;
	}

	i2c_mm_native_get_stats(I2C_DEV, &s);
	twixt_get_stats(&bus);
	LOG_INF("sent %u requests, served %u requests; %u bad; %u retries; lost arbitration %u, "
		"bus timeouts %u, nacks %u, irqs %u",
		bus.sent, bus.served, bad, bus.retries, s.arb_lost, s.bus_timeouts, s.nacks,
		s.irqs);

	/* How much an MCU serves is up to the others: pytest/test_stress.py checks that every
	 * request that was sent was served
	 */
	if (bad == 0) {
		LOG_INF("all settings verified");
	} else {
		LOG_ERR("settings check failed");
	}
	return 0;
}
