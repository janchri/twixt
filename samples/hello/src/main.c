/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hello over twixt: one MCU of two. It serves an echo service of its own,
 * and calls the echo of the other MCU.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <twixt/twixt.h>
#include <twixt/ping.h>

LOG_MODULE_REGISTER(hello, LOG_LEVEL_INF);

#define ECHO_SERVICE TWIXT_SERVICE_APP

/* Runs in the I2C target interrupt: short, and only copying */
static int echo(const struct twixt_req *req, uint8_t *rsp_data)
{
	rsp_data[0] = req->src;
	memcpy(&rsp_data[1], req->data, req->len);
	return 1 + req->len;
}

TWIXT_SERVICE_DEFINE(echo_service, ECHO_SERVICE, echo);

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
	char msg[32];
	uint8_t rsp[1 + sizeof(msg)] = {0};
	int len;
	int ret;

	BUILD_ASSERT(ARRAY_SIZE(mcus) == 2, "hello is for two MCUs");

	LOG_INF("%s at 0x%02x", twixt_self_name(), twixt_self_addr());

	ret = twixt_ping_wait(peer, &info, K_SECONDS(5));
	if (ret) {
		LOG_ERR("no answer from 0x%02x: %d", peer, ret);
		return 0;
	}
	LOG_INF("0x%02x answers: protocol %u", peer, info.version);

	len = snprintf(msg, sizeof(msg), "hello from %s", twixt_self_name());
	ret = twixt_request(peer, ECHO_SERVICE, msg, len, rsp, 1 + len);
	if (ret) {
		LOG_ERR("echo of 0x%02x: %d", peer, ret);
		return 0;
	}
	LOG_INF("0x%02x echoed \"%.*s\", from 0x%02x", peer, len, &rsp[1], rsp[0]);
	return 0;
}
