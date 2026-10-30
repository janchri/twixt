/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ping over twixt (TWIXT_SERVICE_PING): two MCUs, each pings the other until it
 * answers, and logs its protocol version and longest message.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <twixt/twixt.h>
#include <twixt/ping.h>

LOG_MODULE_REGISTER(ping_sample, LOG_LEVEL_INF);

#define PEER                                                                                       \
	(twixt_self_addr() == DT_REG_ADDR(DT_NODELABEL(mcu_a)) ? DT_REG_ADDR(DT_NODELABEL(mcu_b))  \
							       : DT_REG_ADDR(DT_NODELABEL(mcu_a)))

int main(void)
{
	struct twixt_ping_info info;
	int ret;

	LOG_INF("%s at 0x%02x", twixt_self_name(), twixt_self_addr());

	/* Again and again, for a terminal opened later; the other MCU may still boot */
	for (;; k_sleep(K_SECONDS(2))) {
		ret = twixt_ping_wait(PEER, &info, K_SECONDS(5));
		if (ret) {
			LOG_ERR("0x%02x: %d", PEER, ret);
			continue;
		}
		LOG_INF("0x%02x answers: protocol %u, messages up to %u bytes", PEER, info.version,
			info.msg_max);
	}
	return 0;
}
