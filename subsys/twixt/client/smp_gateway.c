/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The MCUmgr of the MCUs for stock SMP tools. One thread serves all front ends, one request at a
 * time, and is the only taker of the responses: a response belongs to the one request under way,
 * by its source and sequence number. The front ends register with
 * SMP_GATEWAY_FRONTEND_DEFINE(): smp_gateway_serial.c, smp_gateway_bt.c.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/sys/byteorder.h>

#include <twixt/service/smp.h>

#include "smp_gateway.h"

LOG_MODULE_REGISTER(twixt_smp_gateway, CONFIG_TWIXT_LOG_LEVEL);

#define OP_MASK      0x07
#define VER_MASK     0x18
#define UPLOAD_ID    1 /* IMG_MGMT_ID_UPLOAD; its header needs MCUboot's, a gateway may not */
#define UPLOAD_TRIES 5
#define UPLOAD_PAUSE K_MSEC(100)

static K_SEM_DEFINE(wake, 0, 1);

void smp_gateway_wake(void)
{
	k_sem_give(&wake);
}

/* An SMP error response, so that a tool hears at once instead of waiting for its own timeout */
static void respond_error(const uint8_t *req, int rc, smp_gateway_respond_fn respond, void *ctx)
{
	uint8_t rsp[SMP_HDR_LEN + 5];

	memcpy(rsp, req, SMP_HDR_LEN);
	rsp[0] = (req[0] & VER_MASK) | ((req[0] & OP_MASK) + 1);
	sys_put_be16(sizeof(rsp) - SMP_HDR_LEN, &rsp[2]);
	rsp[SMP_HDR_LEN + 0] = 0xa1; /* {"rc": rc} */
	rsp[SMP_HDR_LEN + 1] = 0x62;
	rsp[SMP_HDR_LEN + 2] = 'r';
	rsp[SMP_HDR_LEN + 3] = 'c';
	rsp[SMP_HDR_LEN + 4] = rc;
	respond(rsp, sizeof(rsp), ctx);
}

/* An upload says its offset and gets the next one expected, so it may go twice; others may not */
static bool may_send_again(const uint8_t *req)
{
	return (req[0] & OP_MASK) == MGMT_OP_WRITE &&
	       sys_get_be16(&req[4]) == MGMT_GROUP_ID_IMAGE && req[7] == UPLOAD_ID;
}

void smp_gateway_forward(uint8_t addr, struct net_buf *req, smp_gateway_respond_fn respond,
			 void *ctx)
{
	uint8_t hdr[SMP_HDR_LEN];
	int tries = may_send_again(req->data) ? UPLOAD_TRIES : 1;
	struct net_buf *rsp;
	int ret;

	memcpy(hdr, req->data, SMP_HDR_LEN);
	while ((rsp = twixt_smp_rsp_get(NULL, K_NO_WAIT)) != NULL) {
		smp_packet_free(rsp);
	}
	for (int i = 1;; i++) {
		ret = twixt_smp_send(addr, req->data, req->len);
		if (ret == 0 || i == tries) {
			break;
		}
		LOG_WRN("0x%02x: seq %u attempt %d: %d", addr, hdr[6], i, ret);
		k_sleep(UPLOAD_PAUSE);
	}
	smp_packet_free(req);
	if (ret != 0) {
		LOG_ERR("0x%02x: seq %u: %d", addr, hdr[6], ret);
		respond_error(hdr, MGMT_ERR_EUNKNOWN, respond, ctx);
		return;
	}

	k_timepoint_t end = sys_timepoint_calc(K_MSEC(CONFIG_TWIXT_SMP_GATEWAY_RSP_TIMEOUT_MS));

	for (;;) {
		uint8_t src;

		rsp = twixt_smp_rsp_get(&src, sys_timepoint_timeout(end));
		if (rsp == NULL) {
			LOG_ERR("0x%02x: seq %u: no response", addr, hdr[6]);
			respond_error(hdr, MGMT_ERR_ETIMEOUT, respond, ctx);
			return;
		}
		if (src == addr && rsp->len >= SMP_HDR_LEN && rsp->data[6] == hdr[6]) {
			break;
		}
		smp_packet_free(rsp); /* the late answer to an earlier request */
	}
	respond(rsp->data, rsp->len, ctx);
	smp_packet_free(rsp);
}

static void gateway(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	STRUCT_SECTION_FOREACH(smp_gateway_frontend, f) {
		f->start();
	}
	for (;;) {
		k_sem_take(&wake, K_FOREVER);
		STRUCT_SECTION_FOREACH(smp_gateway_frontend, f) {
			f->serve();
		}
	}
}

K_THREAD_DEFINE(twixt_smp_gateway_tid, CONFIG_TWIXT_SMP_GATEWAY_STACK_SIZE, gateway, NULL, NULL,
		NULL, CONFIG_TWIXT_SMP_GATEWAY_PRIORITY, 0, 0);
