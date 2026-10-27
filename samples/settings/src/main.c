/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings over twixt: one MCU of two. It serves its settings (Zephyr
 * settings subsystem, subtree "mcu") to the other MCU, and gets and sets those
 * of the other one by name (twixt/settings.h):
 *
 *   mcu/name       read-only   the devicetree node of the MCU
 *   mcu/greeting   read/write  any text, kept in flash
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>

#include <twixt/twixt.h>
#include <twixt/ping.h>
#include <twixt/settings.h>

LOG_MODULE_REGISTER(settings_sample, LOG_LEVEL_INF);

/*
 * The other MCU's requests run in the I2C target interrupt, and so do these
 * handlers: static, short, and the greeting under a spinlock
 */
static struct k_spinlock lock;
static char greeting[TWIXT_SETTINGS_VALUE_MAX];
static size_t greeting_len;

static int mcu_get(const char *key, char *val, int val_len_max)
{
	int len = -ENOENT;

	if (settings_name_steq(key, "name", NULL)) {
		len = strlen(twixt_self_name());
		memcpy(val, twixt_self_name(), MIN(len, val_len_max));
	} else if (settings_name_steq(key, "greeting", NULL)) {
		K_SPINLOCK(&lock) {
			len = greeting_len;
			memcpy(val, greeting, MIN(len, val_len_max));
		}
	}
	return len;
}

static int mcu_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	char val[sizeof(greeting)];
	ssize_t ret;

	if (settings_name_steq(key, "name", NULL)) {
		return -EACCES;
	}
	if (!settings_name_steq(key, "greeting", NULL)) {
		return -ENOENT;
	}
	if (len > sizeof(val)) {
		return -EINVAL;
	}
	ret = read_cb(cb_arg, val, len);
	if (ret < 0) {
		return ret;
	}
	K_SPINLOCK(&lock) {
		memcpy(greeting, val, len);
		greeting_len = len;
	}
	return 0;
}

static int mcu_export(int (*export_func)(const char *name, const void *val, size_t val_len))
{
	char val[sizeof(greeting)];
	size_t len;

	K_SPINLOCK(&lock) {
		len = greeting_len;
		memcpy(val, greeting, len);
	}
	return len > 0 ? export_func("mcu/greeting", val, len) : 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(mcu, "mcu", mcu_get, mcu_set, NULL, mcu_export);

#define ADDR(node) DT_REG_ADDR(node),
static const uint8_t mcus[] = {DT_FOREACH_STATUS_OKAY(twixt_mcu, ADDR)};

static uint8_t other_mcu(void)
{
	return mcus[0] != twixt_self_addr() ? mcus[0] : mcus[1];
}

int main(void)
{
	uint8_t peer = other_mcu();
	struct twixt_ping_info info;
	char val[TWIXT_SETTINGS_VALUE_MAX];
	char hi[TWIXT_SETTINGS_VALUE_MAX];
	int len;
	int ret;

	BUILD_ASSERT(ARRAY_SIZE(mcus) == 2, "this sample is for two MCUs");

	ret = settings_subsys_init();
	if (ret == 0) {
		ret = settings_load();
	}
	if (ret) {
		LOG_ERR("cannot load the settings: %d", ret);
		return 0;
	}
	LOG_INF("%s at 0x%02x, greeting from flash: \"%.*s\"", twixt_self_name(), twixt_self_addr(),
		(int)greeting_len, greeting);

	while (true) {
		k_sleep(K_SECONDS(5));

		ret = twixt_ping_wait(peer, &info, K_SECONDS(1));
		if (ret) {
			LOG_ERR("no answer from 0x%02x: %d", peer, ret);
			continue;
		}

		len = twixt_settings_get(peer, "mcu/name", val, sizeof(val));
		if (len < 0) {
			LOG_ERR("get mcu/name of 0x%02x: %d", peer, len);
			continue;
		}
		LOG_INF("0x%02x is %.*s", peer, MIN(len, (int)sizeof(val)), val);

		len = snprintf(hi, sizeof(hi), "hi from %s", twixt_self_name());
		ret = twixt_settings_set(peer, "mcu/greeting", hi, MIN(len, (int)sizeof(hi) - 1));
		if (ret) {
			LOG_ERR("set mcu/greeting of 0x%02x: %d", peer, ret);
			continue;
		}

		len = twixt_settings_get(peer, "mcu/greeting", val, sizeof(val));
		if (len < 0) {
			LOG_ERR("get mcu/greeting of 0x%02x: %d", peer, len);
			continue;
		}
		LOG_INF("0x%02x mcu/greeting = \"%.*s\"", peer, MIN(len, (int)sizeof(val)), val);

		ret = twixt_settings_set(peer, "mcu/name", "x", 1);
		LOG_INF("0x%02x mcu/name is read-only: %d", peer, ret);
	}

	return 0;
}
