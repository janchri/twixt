/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Echo over twixt (TWIXT_SERVICE_ECHO): two MCUs, each sends a text to the
 * other's echo service and checks that the same text comes back.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <twixt/twixt.h>
#include <twixt/echo.h>
#include <twixt/ping.h>

LOG_MODULE_REGISTER(echo_sample, LOG_LEVEL_INF);

#define PEER                                                                                       \
	(twixt_self_addr() == DT_REG_ADDR(DT_NODELABEL(mcu_a)) ? DT_REG_ADDR(DT_NODELABEL(mcu_b))  \
							       : DT_REG_ADDR(DT_NODELABEL(mcu_a)))

int main(void)
{
	char text[TWIXT_ECHO_MAX];
	char back[TWIXT_ECHO_MAX];
	struct twixt_ping_info info;
	int len = snprintf(text, sizeof(text), "hello from %s", twixt_self_name());
	int ret;

	/* Again and again, for a terminal opened later; the other MCU may still boot */
	for (;; k_sleep(K_SECONDS(2))) {
		ret = twixt_ping_wait(PEER, &info, K_SECONDS(5));
		if (ret == 0) {
			ret = twixt_echo(PEER, text, len, back);
		}
		if (ret) {
			LOG_ERR("0x%02x: %d", PEER, ret);
		} else if (memcmp(text, back, len) != 0) {
			LOG_ERR("echo from 0x%02x: other bytes", PEER);
		} else {
			LOG_INF("echo from 0x%02x: \"%.*s\"", PEER, len, back);
		}
	}
	return 0;
}
