/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The core of the MCUmgr gateway, with the MCUmgr service replaced by fakes that fail, stay silent
 * or answer late on demand. Its front ends need a tool and a port or a radio: tests/smp_update.
 */

#include <string.h>
#include <zephyr/ztest.h>

/* The options of a gateway, which is not in the devicetree here */
#define CONFIG_TWIXT_SMP_GATEWAY_RSP_TIMEOUT_MS 100
#define CONFIG_TWIXT_SMP_GATEWAY_STACK_SIZE     1024
#define CONFIG_TWIXT_SMP_GATEWAY_PRIORITY       5

#define twixt_smp_send    fake_send
#define twixt_smp_rsp_get fake_rsp_get

#include "../../../subsys/twixt/client/smp_gateway.c"

#define PEER     0x11
#define SEQ      7
#define OTHER_ID 2
#define QUEUE    3

static int sends;
static int fails_left;
static struct net_buf *before_send[QUEUE];
static struct net_buf *after_send[QUEUE];
static uint8_t after_src[QUEUE];
static uint8_t out[64];
static size_t out_len;
static int responses;

static struct net_buf *pop(struct net_buf **queue, uint8_t *src_out, uint8_t *srcs)
{
	for (int i = 0; i < QUEUE; i++) {
		if (queue[i] != NULL) {
			struct net_buf *nb = queue[i];

			queue[i] = NULL;
			if (src_out != NULL && srcs != NULL) {
				*src_out = srcs[i];
			}
			return nb;
		}
	}
	return NULL;
}

int fake_send(uint8_t addr, const uint8_t *pkt, size_t len)
{
	sends++;
	zassert_equal(addr, PEER);
	if (fails_left > 0) {
		fails_left--;
		return -EIO;
	}
	return 0;
}

struct net_buf *fake_rsp_get(uint8_t *src, k_timeout_t timeout)
{
	struct net_buf *nb = pop(before_send, src, NULL);

	if (nb != NULL) {
		return nb;
	}
	if (sends > 0 && (nb = pop(after_send, src, after_src)) != NULL) {
		return nb;
	}
	if (!K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		k_sleep(timeout);
	}
	return NULL;
}

static void respond(const uint8_t *pkt, size_t len, void *ctx)
{
	zassert_true(len <= sizeof(out));
	memcpy(out, pkt, len);
	out_len = len;
	responses++;
	zassert_equal(ctx, &responses);
}

static struct net_buf *packet(uint8_t op, uint16_t group, uint8_t id, uint8_t seq, uint8_t mark)
{
	struct net_buf *nb = smp_packet_alloc();
	uint8_t pkt[SMP_HDR_LEN + 1] = {0};

	zassert_not_null(nb);
	pkt[0] = (1 << 3) | op;
	sys_put_be16(1, &pkt[2]);
	sys_put_be16(group, &pkt[4]);
	pkt[6] = seq;
	pkt[7] = id;
	pkt[SMP_HDR_LEN] = mark;
	net_buf_add_mem(nb, pkt, sizeof(pkt));
	return nb;
}

static struct net_buf *upload(void)
{
	return packet(MGMT_OP_WRITE, MGMT_GROUP_ID_IMAGE, UPLOAD_ID, SEQ, 0);
}

static struct net_buf *other_request(void)
{
	return packet(MGMT_OP_WRITE, MGMT_GROUP_ID_IMAGE, OTHER_ID, SEQ, 0);
}

static struct net_buf *answer(uint8_t seq, uint8_t mark)
{
	return packet(MGMT_OP_WRITE_RSP, MGMT_GROUP_ID_IMAGE, UPLOAD_ID, seq, mark);
}

static void forward(struct net_buf *req)
{
	smp_gateway_forward(PEER, req, respond, &responses);
}

static void assert_error(int rc)
{
	zassert_equal(responses, 1);
	zassert_equal(out_len, SMP_HDR_LEN + 5);
	zassert_equal(out[0], (1 << 3) | MGMT_OP_WRITE_RSP, "the op of a response");
	zassert_equal(sys_get_be16(&out[2]), 5, "the length of the body");
	zassert_equal(out[6], SEQ, "the sequence number of the request");
	zassert_equal(out[SMP_HDR_LEN], 0xa1, "a map of one");
	zassert_mem_equal(&out[SMP_HDR_LEN + 2], "rc", 2);
	zassert_equal(out[SMP_HDR_LEN + 4], rc);
}

static void reset(void *fixture)
{
	struct net_buf *nb;

	sends = 0;
	fails_left = 0;
	out_len = 0;
	responses = 0;
	while ((nb = pop(before_send, NULL, NULL)) != NULL) {
		smp_packet_free(nb);
	}
	while ((nb = pop(after_send, NULL, NULL)) != NULL) {
		smp_packet_free(nb);
	}
}

ZTEST(smp_gateway, test_response_passes_through)
{
	after_send[0] = answer(SEQ, 0x55);
	after_src[0] = PEER;
	forward(upload());

	zassert_equal(sends, 1);
	zassert_equal(responses, 1);
	zassert_equal(out_len, SMP_HDR_LEN + 1);
	zassert_equal(out[SMP_HDR_LEN], 0x55);
}

ZTEST(smp_gateway, test_upload_is_sent_again)
{
	fails_left = 2;
	after_send[0] = answer(SEQ, 0x55);
	after_src[0] = PEER;
	forward(upload());

	zassert_equal(sends, 3);
	zassert_equal(responses, 1);
	zassert_equal(out[SMP_HDR_LEN], 0x55);
}

ZTEST(smp_gateway, test_upload_gives_up)
{
	fails_left = UPLOAD_TRIES;
	forward(upload());

	zassert_equal(sends, UPLOAD_TRIES);
	assert_error(MGMT_ERR_EUNKNOWN);
}

ZTEST(smp_gateway, test_other_request_goes_once)
{
	fails_left = 1;
	forward(other_request());

	zassert_equal(sends, 1);
	assert_error(MGMT_ERR_EUNKNOWN);
}

ZTEST(smp_gateway, test_read_of_image_group_goes_once)
{
	fails_left = 1;
	forward(packet(MGMT_OP_READ, MGMT_GROUP_ID_IMAGE, UPLOAD_ID, SEQ, 0));

	zassert_equal(sends, 1);
}

ZTEST(smp_gateway, test_upload_of_other_group_goes_once)
{
	fails_left = 1;
	forward(packet(MGMT_OP_WRITE, MGMT_GROUP_ID_OS, UPLOAD_ID, SEQ, 0));

	zassert_equal(sends, 1);
}

ZTEST(smp_gateway, test_no_response)
{
	int64_t start = k_uptime_get();

	forward(upload());

	assert_error(MGMT_ERR_ETIMEOUT);
	zassert_true(k_uptime_get() - start >= CONFIG_TWIXT_SMP_GATEWAY_RSP_TIMEOUT_MS);
}

ZTEST(smp_gateway, test_other_sequence_number_is_ignored)
{
	after_send[0] = answer(SEQ - 1, 0x11);
	after_src[0] = PEER;
	after_send[1] = answer(SEQ, 0x55);
	after_src[1] = PEER;
	forward(upload());

	zassert_equal(responses, 1);
	zassert_equal(out[SMP_HDR_LEN], 0x55);
}

ZTEST(smp_gateway, test_other_source_is_ignored)
{
	after_send[0] = answer(SEQ, 0x11);
	after_src[0] = PEER + 1;
	after_send[1] = answer(SEQ, 0x55);
	after_src[1] = PEER;
	forward(upload());

	zassert_equal(responses, 1);
	zassert_equal(out[SMP_HDR_LEN], 0x55);
}

/* What waits from before the request is not its answer, even with its sequence number */
ZTEST(smp_gateway, test_old_responses_are_dropped)
{
	before_send[0] = answer(SEQ, 0x11);
	after_send[0] = answer(SEQ, 0x55);
	after_src[0] = PEER;
	forward(upload());

	zassert_equal(responses, 1);
	zassert_equal(out[SMP_HDR_LEN], 0x55);
}

ZTEST(smp_gateway, test_packets_are_freed)
{
	for (int i = 0; i < 2 * CONFIG_MCUMGR_TRANSPORT_NETBUF_COUNT; i++) {
		responses = 0;
		sends = 0;
		after_send[0] = answer(SEQ, 0x55);
		after_src[0] = PEER;
		forward(upload());
		zassert_equal(responses, 1, "round %d", i);
	}
}

static atomic_t starts;
static K_SEM_DEFINE(served, 0, 1);

static void frontend_start(void)
{
	atomic_inc(&starts);
}

static void frontend_serve(void)
{
	k_sem_give(&served);
}

SMP_GATEWAY_FRONTEND_DEFINE(test_frontend, frontend_start, frontend_serve);

ZTEST(smp_gateway, test_frontend_runs_on_the_thread)
{
	/* The gateway's thread runs once this one lets it */
	zassert_true(WAIT_FOR(atomic_get(&starts) == 1, USEC_PER_SEC, k_msleep(1)));
	k_sem_reset(&served);
	smp_gateway_wake();
	zassert_ok(k_sem_take(&served, K_SECONDS(1)));
}

ZTEST_SUITE(smp_gateway, NULL, NULL, reset, NULL, NULL);
