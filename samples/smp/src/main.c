/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUmgr over twixt (TWIXT_SERVICE_SMP): two MCUs with MCUmgr on the twixt
 * MCUmgr service. b sends a an os echo request, and a's MCUmgr answers.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/sys/byteorder.h>

#include <twixt/twixt.h>
#include <twixt/ping.h>
#include <twixt/smp.h>

LOG_MODULE_REGISTER(smp_sample, LOG_LEVEL_INF);

#define SERVER  DT_REG_ADDR(DT_NODELABEL(mcu_a))
#define TEXT    "hello over the bus"
#define HDR_LEN 8

BUILD_ASSERT(sizeof(TEXT) - 1 < 24, "a short CBOR text");

/* os echo {"d": TEXT}: SMP v2, write, group 0, command 0 */
static size_t echo_request(uint8_t *pkt)
{
	static const uint8_t cbor[] = {0xa1, 0x61, 'd', 0x60 + sizeof(TEXT) - 1};
	size_t len = sizeof(cbor) + sizeof(TEXT) - 1;

	memset(pkt, 0, HDR_LEN);
	pkt[0] = (1 << 3) | 2;
	sys_put_be16(len, &pkt[2]);
	memcpy(&pkt[HDR_LEN], cbor, sizeof(cbor));
	memcpy(&pkt[HDR_LEN + sizeof(cbor)], TEXT, sizeof(TEXT) - 1);
	return HDR_LEN + len;
}

/* MCUmgr encodes the response map with indefinite length: the text is inside */
static bool contains(const struct net_buf *nb, const char *text)
{
	for (size_t i = HDR_LEN; i + strlen(text) <= nb->len; i++) {
		if (memcmp(&nb->data[i], text, strlen(text)) == 0) {
			return true;
		}
	}
	return false;
}

int main(void)
{
	uint8_t pkt[HDR_LEN + 4 + sizeof(TEXT)];
	struct twixt_ping_info info;
	struct net_buf *nb;
	uint8_t src;
	int ret;

	LOG_INF("%s at 0x%02x: MCUmgr on the twixt service", twixt_self_name(), twixt_self_addr());
	if (twixt_self_addr() == SERVER) {
		return 0;
	}

	/* Again and again, for a terminal opened later; the server may still boot */
	for (;; k_sleep(K_SECONDS(2))) {
		ret = twixt_ping_wait(SERVER, &info, K_SECONDS(5));
		if (ret == 0) {
			ret = twixt_smp_send(SERVER, pkt, echo_request(pkt));
		}
		if (ret) {
			LOG_ERR("MCUmgr echo: %d", ret);
			continue;
		}
		nb = twixt_smp_rsp_get(&src, K_SECONDS(5));
		if (nb == NULL || src != SERVER || !contains(nb, TEXT)) {
			LOG_ERR("MCUmgr echo: no answer");
		} else {
			LOG_INF("MCUmgr echo from 0x%02x: %s", src, TEXT);
		}
		if (nb != NULL) {
			smp_packet_free(nb);
		}
	}
	return 0;
}
