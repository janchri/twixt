/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A service of your own (TWIXT_SERVICE_APP): two MCUs, both serve "add", and
 * each asks the other for a sum.
 *
 *     request:  int32 a, int32 b (little endian)
 *     response: int32 a + b
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <twixt/twixt.h>
#include <twixt/ping.h>

LOG_MODULE_REGISTER(app_sample, LOG_LEVEL_INF);

#define ADD_SERVICE TWIXT_SERVICE_APP

#define PEER                                                                                       \
	(twixt_self_addr() == DT_REG_ADDR(DT_NODELABEL(mcu_a)) ? DT_REG_ADDR(DT_NODELABEL(mcu_b))  \
							       : DT_REG_ADDR(DT_NODELABEL(mcu_a)))

/* Runs in the I2C target interrupt: short, no waiting */
static int add(const struct twixt_req *req, uint8_t *rsp_data)
{
	if (req->len != 8) {
		return -EINVAL;
	}
	sys_put_le32(sys_get_le32(&req->data[0]) + sys_get_le32(&req->data[4]), rsp_data);
	return 4;
}

TWIXT_SERVICE_DEFINE(add_service, ADD_SERVICE, add);

static int add_on(uint8_t addr, int32_t a, int32_t b, int32_t *sum)
{
	uint8_t req[8];
	uint8_t rsp[4];
	int ret;

	sys_put_le32(a, &req[0]);
	sys_put_le32(b, &req[4]);
	ret = twixt_request(addr, ADD_SERVICE, req, sizeof(req), rsp, sizeof(rsp));
	if (ret == 0) {
		*sum = sys_get_le32(rsp);
	}
	return ret;
}

int main(void)
{
	bool is_a = twixt_self_addr() == DT_REG_ADDR(DT_NODELABEL(mcu_a));
	int32_t a = is_a ? 2 : 40;
	int32_t b = is_a ? 3 : 2;
	struct twixt_ping_info info;
	int32_t sum;
	int ret;

	/* Again and again, for a terminal opened later; the other MCU may still boot */
	for (;; k_sleep(K_SECONDS(2))) {
		ret = twixt_ping_wait(PEER, &info, K_SECONDS(5));
		if (ret == 0) {
			ret = add_on(PEER, a, b, &sum);
		}
		if (ret) {
			LOG_ERR("0x%02x: %d", PEER, ret);
			continue;
		}
		LOG_INF("0x%02x adds %d + %d = %d", PEER, a, b, sum);
	}
	return 0;
}
