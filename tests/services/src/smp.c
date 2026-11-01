/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The MCUmgr service, to this MCU itself: an os echo request goes in
 * fragments to this MCU's MCUmgr, whose response comes back the same way. The
 * fault tests send the fragments one by one, as twixt_smp_send() would.
 */

#include <string.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <twixt/service/smp.h>
#include <twixt/twixt.h>

#define ADDR       DT_REG_ADDR(TWIXT_SELF)
#define HDR_LEN    8
#define OP_WRITE   2
#define OP_WRITE_R 3
#define SMP_V2     (1 << 3)
#define CBOR_LEN   6 /* {"d": text} without the text, text of 24 bytes or more */
#define TEXT_LONG  300
#define TEXT_FULL  (CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE - HDR_LEN - CBOR_LEN)
#define WAIT       K_MSEC(500)
#define OP_INVALID 5
#define TIMEOUT    CONFIG_TWIXT_SMP_TIMEOUT_MS

static uint8_t pkt[CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE];
static char text[TEXT_FULL + 1];
static uint8_t seq;

static size_t echo_request(const char *text)
{
	size_t n = strlen(text);
	size_t len = 0;
	uint8_t *p = &pkt[HDR_LEN];

	p[len++] = 0xa1;
	p[len++] = 0x61;
	p[len++] = 'd';
	if (n < 24) {
		p[len++] = 0x60 + n;
	} else {
		p[len++] = 0x79;
		sys_put_be16(n, &p[len]);
		len += 2;
	}
	memcpy(&p[len], text, n);
	len += n;

	pkt[0] = SMP_V2 | OP_WRITE;
	pkt[1] = 0;
	sys_put_be16(len, &pkt[2]);
	sys_put_be16(0, &pkt[4]);
	pkt[6] = ++seq;
	pkt[7] = 0;
	return HDR_LEN + len;
}

/* MCUmgr encodes the response map with indefinite length: the text is inside, not at the end */
static bool contains(const struct net_buf *nb, const char *text)
{
	size_t n = strlen(text);

	for (size_t i = HDR_LEN; i + n <= nb->len; i++) {
		if (memcmp(&nb->data[i], text, n) == 0) {
			return true;
		}
	}
	return false;
}

static struct net_buf *expect_response(void)
{
	uint8_t src;
	struct net_buf *nb = twixt_smp_rsp_get(&src, WAIT);

	zassert_not_null(nb, "no response");
	zassert_equal(src, ADDR);
	zassert_true(nb->len > HDR_LEN);
	zassert_equal(nb->data[0] & 0x07, OP_WRITE_R);
	zassert_equal(nb->data[6], seq);
	zassert_equal(sys_get_be16(&nb->data[2]), nb->len - HDR_LEN);
	return nb;
}

static void expect_echo(const char *text)
{
	struct net_buf *nb = expect_response();

	zassert_true(contains(nb, text), "the response has another text");
	smp_packet_free(nb);
}

static void expect_none(void)
{
	struct net_buf *nb = twixt_smp_rsp_get(NULL, WAIT);

	if (nb != NULL) {
		smp_packet_free(nb);
	}
	zassert_is_null(nb, "a response to a broken request");
}

/* Fragment i of pkt and its CRC, as twixt_smp_send() cuts them */
static int send_frag(size_t len, size_t i, bool crc_ok)
{
	static uint8_t stream[sizeof(pkt) + 2];
	uint8_t frag[1 + TWIXT_SMP_FRAG_MAX];
	size_t total = len + 2;
	size_t off = i * TWIXT_SMP_FRAG_MAX;
	size_t n = MIN(total - off, TWIXT_SMP_FRAG_MAX);

	memcpy(stream, pkt, len);
	sys_put_be16(crc16_itu_t(0, pkt, len) ^ (crc_ok ? 0 : 1), &stream[len]);
	frag[0] = (i == 0 ? TWIXT_SMP_FIRST : 0) | i;
	memcpy(&frag[1], &stream[off], n);
	return twixt_request(ADDR, TWIXT_SERVICE_SMP, frag, 1 + n, NULL, 0);
}

static size_t frags(size_t len)
{
	return DIV_ROUND_UP(len + 2, TWIXT_SMP_FRAG_MAX);
}

static const char *long_text(size_t n)
{
	for (size_t i = 0; i < n; i++) {
		text[i] = 'a' + i % 26;
	}
	text[n] = '\0';
	return text;
}

static void before(void *fixture)
{
	struct net_buf *nb;

	while ((nb = twixt_smp_rsp_get(NULL, K_NO_WAIT)) != NULL) {
		smp_packet_free(nb);
	}
}

ZTEST(twixt_smp, test_echo)
{
	size_t len = echo_request("hello");

	zassert_equal(frags(len), 1);
	zassert_ok(twixt_smp_send(ADDR, pkt, len));
	expect_echo("hello");
}

ZTEST(twixt_smp, test_echo_long)
{
	size_t len = echo_request(long_text(TEXT_LONG));

	zassert_true(frags(len) > 4);
	zassert_ok(twixt_smp_send(ADDR, pkt, len));
	expect_echo(text);
}

ZTEST(twixt_smp, test_fragment_twice)
{
	size_t len = echo_request(long_text(TEXT_LONG));

	for (size_t i = 0; i < frags(len); i++) {
		zassert_ok(send_frag(len, i, true));
		if (i == 1) {
			zassert_ok(send_frag(len, i, true));
		}
	}
	expect_echo(text);
}

ZTEST(twixt_smp, test_fragment_missing)
{
	size_t len = echo_request(long_text(TEXT_LONG));

	zassert_ok(send_frag(len, 0, true));
	zassert_equal(send_frag(len, 2, true), -EPROTO);
	expect_none();

	zassert_ok(twixt_smp_send(ADDR, pkt, len));
	expect_echo(text);
}

ZTEST(twixt_smp, test_crc_wrong)
{
	size_t len = echo_request("hello");

	zassert_ok(send_frag(len, 0, false));
	expect_none();
}

ZTEST(twixt_smp, test_no_first)
{
	uint8_t frag[] = {1, 0};

	zassert_equal(twixt_request(ADDR, TWIXT_SERVICE_SMP, frag, sizeof(frag), NULL, 0), -EPROTO);
	expect_none();
}

/*
 * A request as large as MCUmgr's buffer fits, the CRC outside it, and gets a response. Not the
 * echo: that would be a byte longer than the buffer (indefinite-length map), so MCUmgr answers
 * with an error.
 */
ZTEST(twixt_smp, test_full_buffer)
{
	size_t len = echo_request(long_text(TEXT_FULL));

	zassert_equal(len, CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE);
	zassert_ok(twixt_smp_send(ADDR, pkt, len));
	smp_packet_free(expect_response());
}

/* A FIRST sent again starts the packet anew: a packet of one fragment runs twice */
ZTEST(twixt_smp, test_single_fragment_twice)
{
	size_t len = echo_request("hello");

	zassert_equal(frags(len), 1);
	zassert_ok(send_frag(len, 0, true));
	zassert_ok(send_frag(len, 0, true));
	expect_echo("hello");
	expect_echo("hello");
	expect_none();
}

/* The last fragment sent again is refused, although its packet went through */
ZTEST(twixt_smp, test_last_fragment_twice)
{
	size_t len = echo_request(long_text(TEXT_LONG));
	size_t last = frags(len) - 1;

	for (size_t i = 0; i <= last; i++) {
		zassert_ok(send_frag(len, i, true));
	}
	zassert_equal(send_frag(len, last, true), -EPROTO);
	expect_echo(text);
	expect_none();
}

static void expect_pool_full(void)
{
	struct net_buf *nb[CONFIG_MCUMGR_TRANSPORT_NETBUF_COUNT];

	for (size_t i = 0; i < ARRAY_SIZE(nb); i++) {
		nb[i] = smp_packet_alloc();
		zassert_not_null(nb[i], "buffer %zu is still held", i);
	}
	for (size_t i = 0; i < ARRAY_SIZE(nb); i++) {
		smp_packet_free(nb[i]);
	}
}

/* A sender gone in the middle of a packet leaves its buffer behind only for a while */
ZTEST(twixt_smp, test_abandoned_packet_expires)
{
	size_t len = echo_request(long_text(TEXT_LONG));
	struct twixt_smp_stats s0, s1;

	twixt_smp_get_stats(&s0);
	zassert_ok(send_frag(len, 0, true));
	k_msleep(2 * TIMEOUT + 50);
	twixt_smp_get_stats(&s1);
	zassert_equal(s1.expired, s0.expired + 1);
	expect_pool_full();
	zassert_equal(send_frag(len, 1, true), -EPROTO);
}

ZTEST(twixt_smp, test_slow_packet_stays)
{
	size_t len = echo_request(long_text(TEXT_LONG));
	struct twixt_smp_stats s0, s1;

	twixt_smp_get_stats(&s0);
	for (size_t i = 0; i < frags(len); i++) {
		zassert_ok(send_frag(len, i, true));
		k_msleep(TIMEOUT * 3 / 4);
	}
	expect_echo(text);
	twixt_smp_get_stats(&s1);
	zassert_equal(s1.expired, s0.expired);
}

/* A length no buffer holds is refused with the header, not when the buffer is full */
ZTEST(twixt_smp, test_impossible_length)
{
	pkt[0] = SMP_V2 | OP_WRITE;
	sys_put_be16(CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE - HDR_LEN + 1, &pkt[2]);
	zassert_equal(send_frag(HDR_LEN, 0, true), -EMSGSIZE);
	expect_none();
	expect_pool_full();
}

static size_t bare_packet(uint8_t op)
{
	memset(pkt, 0, HDR_LEN);
	pkt[0] = SMP_V2 | op;
	return HDR_LEN;
}

ZTEST(twixt_smp, test_invalid_op)
{
	size_t len = echo_request("hello");

	zassert_ok(twixt_smp_send(ADDR, pkt, len));
	expect_echo("hello");

	len = bare_packet(OP_INVALID);
	zassert_ok(twixt_smp_send(ADDR, pkt, len));
	expect_none();
	expect_pool_full();
}

/* Responses nobody takes hold a bounded number of buffers */
ZTEST(twixt_smp, test_responses_bounded)
{
	size_t len = echo_request("hello");
	struct twixt_smp_stats s0, s1;

	zassert_ok(twixt_smp_send(ADDR, pkt, len));
	expect_echo("hello");

	twixt_smp_get_stats(&s0);
	len = bare_packet(OP_WRITE_R);
	for (int i = 0; i < CONFIG_TWIXT_SMP_RSP_MAX + 3; i++) {
		zassert_ok(twixt_smp_send(ADDR, pkt, len));
	}
	twixt_smp_get_stats(&s1);
	zassert_equal(s1.rsp_dropped, s0.rsp_dropped + 3);

	for (int i = 0; i < CONFIG_TWIXT_SMP_RSP_MAX; i++) {
		struct net_buf *nb = twixt_smp_rsp_get(NULL, K_NO_WAIT);

		zassert_not_null(nb);
		smp_packet_free(nb);
	}
	zassert_is_null(twixt_smp_rsp_get(NULL, K_NO_WAIT));
	expect_pool_full();

	len = echo_request("hello");
	zassert_ok(twixt_smp_send(ADDR, pkt, len));
	expect_echo("hello");
}

ZTEST_SUITE(twixt_smp, NULL, NULL, before, NULL, NULL);
