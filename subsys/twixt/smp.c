/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUmgr (SMP) over twixt, see <twixt/smp.h>. Fragments arrive in the I2C
 * target interrupt: a packet is put together per sender, then handed to
 * MCUmgr (requests) or to twixt_smp_rsp_get() (responses).
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <mgmt/mcumgr/transport/smp_internal.h>

#include <twixt/smp.h>
#include <twixt/twixt.h>

LOG_MODULE_REGISTER(twixt_smp, CONFIG_TWIXT_LOG_LEVEL);

#define SMP_HDR_LEN 8
#define CRC_LEN     (IS_ENABLED(CONFIG_TWIXT_SMP_CRC) ? 2 : 0)
#define INDEX_MASK  0x7f

struct peer {
	struct net_buf *nb; /* the packet coming in, NULL: none */
	size_t len;         /* of the whole packet, 0 until its header is in */
	uint8_t crc[2];     /* outside nb, so that a packet as large as MCUmgr's buffer fits */
	uint8_t crc_len;
	uint8_t addr;
	uint8_t next; /* index of the fragment expected next */
};

static struct k_spinlock lock;
static struct peer peers[CONFIG_TWIXT_SMP_PEERS];
static struct smp_transport transport;
static K_FIFO_DEFINE(rsp_fifo);
static K_MUTEX_DEFINE(send_lock);
static atomic_t client;

static bool is_request(const struct net_buf *nb)
{
	uint8_t op = nb->data[0] & 0x07;

	return op == MGMT_OP_READ || op == MGMT_OP_WRITE;
}

static void drop(struct peer *p)
{
	if (p->nb != NULL) {
		smp_packet_free(p->nb);
		p->nb = NULL;
	}
}

static struct peer *peer_of(uint8_t addr)
{
	struct peer *free = NULL;

	for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
		if (peers[i].nb != NULL && peers[i].addr == addr) {
			return &peers[i];
		}
		if (peers[i].nb == NULL && free == NULL) {
			free = &peers[i];
		}
	}
	return free;
}

/* A whole packet: CRC off, then to MCUmgr or to the responses */
static void deliver(struct peer *p)
{
	struct net_buf *nb = p->nb;

	p->nb = NULL;
	if (IS_ENABLED(CONFIG_TWIXT_SMP_CRC)) {
		if (crc16_itu_t(0, nb->data, nb->len) != sys_get_be16(p->crc)) {
			LOG_WRN("0x%02x: CRC wrong, packet dropped", p->addr);
			smp_packet_free(nb);
			return;
		}
	}
	*(uint8_t *)net_buf_user_data(nb) = p->addr;
	if (is_request(nb)) {
		smp_rx_req(&transport, nb);
	} else if (atomic_get(&client)) {
		k_fifo_put(&rsp_fifo, nb);
	} else {
		smp_packet_free(nb);
	}
}

static int serve_locked(const struct twixt_req *req)
{
	uint8_t hdr = req->data[0];
	uint8_t index = hdr & INDEX_MASK;
	struct peer *p = peer_of(req->src);
	size_t len = req->len - 1;

	if (p == NULL) {
		return -ENOMEM;
	}
	if (hdr & TWIXT_SMP_FIRST) {
		drop(p);
		p->nb = smp_packet_alloc();
		if (p->nb == NULL) {
			return -ENOMEM;
		}
		p->addr = req->src;
		p->next = 0;
		p->len = 0;
		p->crc_len = 0;
	} else if (p->nb == NULL) {
		return -EPROTO;
	} else if (index == ((p->next - 1) & INDEX_MASK)) {
		return 0; /* sent again after a lost answer */
	}
	if (index != p->next) {
		drop(p);
		return -EPROTO;
	}
	p->next = (p->next + 1) & INDEX_MASK;

	for (size_t i = 0; i < len; i++) {
		uint8_t c = req->data[1 + i];

		if (p->len == 0 || p->nb->len < p->len) {
			if (net_buf_tailroom(p->nb) == 0) {
				drop(p);
				return -EMSGSIZE;
			}
			net_buf_add_u8(p->nb, c);
			if (p->nb->len == SMP_HDR_LEN) {
				p->len = SMP_HDR_LEN + sys_get_be16(&p->nb->data[2]);
			}
		} else if (p->crc_len < CRC_LEN) {
			p->crc[p->crc_len++] = c;
		} else {
			drop(p);
			return -EPROTO;
		}
	}
	if (p->len != 0 && p->nb->len == p->len && p->crc_len == CRC_LEN) {
		deliver(p);
	}
	return 0;
}

static int serve(const struct twixt_req *req, uint8_t *rsp_data)
{
	int ret;

	if (req->len < 1) {
		return -EINVAL;
	}
	K_SPINLOCK(&lock) {
		ret = serve_locked(req);
	}
	return ret;
}

TWIXT_SERVICE_DEFINE(twixt_smp_service, TWIXT_SERVICE_SMP, serve);

int twixt_smp_send(uint8_t addr, const uint8_t *pkt, size_t len)
{
	uint8_t crc[2];
	uint8_t frag[1 + TWIXT_SMP_FRAG_MAX];
	size_t total = len + CRC_LEN;
	int ret = 0;

	if (IS_ENABLED(CONFIG_TWIXT_SMP_CRC)) {
		sys_put_be16(crc16_itu_t(0, pkt, len), crc);
	}
	if (len >= 1 && ((pkt[0] & 0x07) == MGMT_OP_READ || (pkt[0] & 0x07) == MGMT_OP_WRITE)) {
		atomic_set(&client, 1);
	}

	k_mutex_lock(&send_lock, K_FOREVER);
	for (size_t off = 0, i = 0; ret == 0 && off < total; i++) {
		size_t n = MIN(total - off, TWIXT_SMP_FRAG_MAX);

		frag[0] = (off == 0 ? TWIXT_SMP_FIRST : 0) | (i & INDEX_MASK);
		for (size_t j = 0; j < n; j++) {
			frag[1 + j] = off + j < len ? pkt[off + j] : crc[off + j - len];
		}
		ret = twixt_request(addr, TWIXT_SERVICE_SMP, frag, 1 + n, NULL, 0);
		off += n;
	}
	k_mutex_unlock(&send_lock);
	return ret;
}

struct net_buf *twixt_smp_rsp_get(uint8_t *src, k_timeout_t timeout)
{
	struct net_buf *nb = k_fifo_get(&rsp_fifo, timeout);

	if (nb != NULL && src != NULL) {
		*src = *(uint8_t *)net_buf_user_data(nb);
	}
	return nb;
}

/* MCUmgr's response goes back to the MCU that sent the request */
static int output(struct net_buf *nb)
{
	int ret = twixt_smp_send(*(uint8_t *)net_buf_user_data(nb), nb->data, nb->len);

	smp_packet_free(nb);
	return ret ? MGMT_ERR_EUNKNOWN : MGMT_ERR_EOK;
}

static int ud_copy(struct net_buf *dst, const struct net_buf *src)
{
	*(uint8_t *)net_buf_user_data(dst) = *(const uint8_t *)net_buf_user_data(src);
	return 0;
}

static void twixt_smp_setup(void)
{
	transport.functions.output = output;
	transport.functions.ud_copy = ud_copy;
	if (smp_transport_init(&transport) != 0) {
		LOG_ERR("SMP transport init failed");
	}
}

MCUMGR_HANDLER_DEFINE(twixt_smp, twixt_smp_setup);
