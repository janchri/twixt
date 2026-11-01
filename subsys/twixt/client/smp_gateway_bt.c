/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Bluetooth front end of the MCUmgr gateway (twixt,smp-gateway-bt): to a tool, one device per
 * MCU, each with the standard SMP service. An advertising set and an identity per MCU; the
 * identity a connection came in on says the MCU it is for. A connection without SMP traffic is
 * dropped: a tool that died can leave the link up, and the MCU unreachable with it.
 */

#define DT_DRV_COMPAT twixt_smp_gateway_bt

#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#include <zephyr/sys/byteorder.h>

#include "smp_gateway.h"

LOG_MODULE_REGISTER(twixt_smp_gateway_bt, CONFIG_TWIXT_LOG_LEVEL);

#define NOTIFY_TRIES 200
#define NOTIFY_PAUSE K_MSEC(5)
#define LINKS        DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)
#define IDLE_TIMEOUT K_MSEC(CONFIG_TWIXT_SMP_GATEWAY_BT_IDLE_MS)

BUILD_ASSERT(LINKS <= CONFIG_BT_MAX_CONN, "a connection per twixt,smp-gateway-bt");
BUILD_ASSERT(LINKS <= CONFIG_BT_ID_MAX, "an identity per twixt,smp-gateway-bt");
BUILD_ASSERT(LINKS <= CONFIG_BT_EXT_ADV_MAX_ADV_SET, "a set per twixt,smp-gateway-bt");

struct link {
	const char *name;
	uint8_t addr;
	uint8_t id;
	struct bt_le_ext_adv *adv;
	struct bt_conn *conn;
	struct net_buf *in;    /* the packet coming in */
	struct net_buf *ready; /* a whole one for the gateway's thread */
	struct k_timer idle;
	atomic_t idle_expired;
};

#define LINK_INIT(n)                                                                               \
	{                                                                                          \
		.name = DT_INST_PROP_OR(n, bt_name, DT_NODE_FULL_NAME(DT_INST_PHANDLE(n, peer))),  \
		.addr = DT_REG_ADDR(DT_INST_PHANDLE(n, peer)),                                     \
	},

static struct link links[] = {DT_INST_FOREACH_STATUS_OKAY(LINK_INIT)};
static struct k_spinlock lock;
static atomic_t advertise_again;

static struct link *link_of(const struct bt_conn *conn)
{
	for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
		if (links[i].conn == conn) {
			return &links[i];
		}
	}
	return NULL;
}

/* A tool may cut a packet to the MTU: its header says when it is whole */
static ssize_t chr_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags)
{
	struct link *l = link_of(conn);
	bool whole = false;

	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);
	if (l == NULL) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}
	k_timer_start(&l->idle, IDLE_TIMEOUT, K_NO_WAIT);
	K_SPINLOCK(&lock) {
		if (l->in == NULL) {
			l->in = smp_packet_alloc();
		}
		if (l->in == NULL || net_buf_tailroom(l->in) < len) {
			LOG_WRN("%s: packet dropped", l->name);
			if (l->in != NULL) {
				smp_packet_free(l->in);
				l->in = NULL;
			}
			K_SPINLOCK_BREAK;
		}
		net_buf_add_mem(l->in, buf, len);
		if (l->in->len >= SMP_HDR_LEN &&
		    l->in->len >= SMP_HDR_LEN + sys_get_be16(&l->in->data[2])) {
			if (l->ready != NULL) {
				smp_packet_free(l->ready); /* a tool that did not wait */
			}
			l->ready = l->in;
			l->in = NULL;
			whole = true;
		}
	}
	if (whole) {
		smp_gateway_wake();
	}
	return len;
}

BT_GATT_SERVICE_DEFINE(gateway_svc, BT_GATT_PRIMARY_SERVICE(SMP_BT_SVC_UUID),
		       BT_GATT_CHARACTERISTIC(SMP_BT_CHR_UUID,
					      BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
					      BT_GATT_PERM_WRITE, NULL, chr_write, NULL),
		       BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

static struct bt_conn *conn_get(struct link *l)
{
	struct bt_conn *conn = NULL;

	K_SPINLOCK(&lock) {
		if (l->conn != NULL) {
			conn = bt_conn_ref(l->conn);
		}
	}
	return conn;
}

static void respond(const uint8_t *pkt, size_t len, void *ctx)
{
	struct link *l = ctx;
	struct bt_conn *conn = conn_get(l);

	if (conn == NULL) {
		return;
	}
	for (size_t off = 0; off < len;) {
		size_t n = MIN(len - off, bt_gatt_get_mtu(conn) - 3);
		int ret = -ENOMEM;

		for (int i = 0; i < NOTIFY_TRIES && ret == -ENOMEM; i++) {
			ret = bt_gatt_notify(conn, &gateway_svc.attrs[2], &pkt[off], n);
			if (ret == -ENOMEM) {
				k_sleep(NOTIFY_PAUSE);
			}
		}
		if (ret != 0) {
			LOG_WRN("%s: response lost: %d", l->name, ret);
			break;
		}
		off += n;
	}
	bt_conn_unref(conn);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;

	if (err != 0 || bt_conn_get_info(conn, &info) != 0) {
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
		if (links[i].id == info.id) {
			K_SPINLOCK(&lock) {
				links[i].conn = bt_conn_ref(conn);
			}
			k_timer_start(&links[i].idle, IDLE_TIMEOUT, K_NO_WAIT);
			LOG_INF("%s: connected", links[i].name);
			return;
		}
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct link *l = link_of(conn);

	if (l == NULL) {
		return;
	}
	k_timer_stop(&l->idle);
	K_SPINLOCK(&lock) {
		bt_conn_unref(l->conn);
		l->conn = NULL;
		if (l->in != NULL) {
			smp_packet_free(l->in);
			l->in = NULL;
		}
		if (l->ready != NULL) {
			smp_packet_free(l->ready);
			l->ready = NULL;
		}
	}
	LOG_INF("%s: disconnected: 0x%02x", l->name, reason);
}

static void advertise(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
		if (links[i].conn == NULL) {
			int ret = bt_le_ext_adv_start(links[i].adv, BT_LE_EXT_ADV_START_DEFAULT);

			if (ret != 0 && ret != -EALREADY) {
				LOG_ERR("%s: advertising: %d", links[i].name, ret);
			}
		}
	}
}

/*
 * A set stops advertising when it gets its connection: on again once that one is gone. Not from
 * here: this runs on the system workqueue, where the host does not allow the HCI commands.
 */
static void recycled(void)
{
	atomic_set(&advertise_again, 1);
	smp_gateway_wake();
}

BT_CONN_CB_DEFINE(gateway_conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

static void idle_expired(struct k_timer *timer)
{
	struct link *l = CONTAINER_OF(timer, struct link, idle);

	atomic_set(&l->idle_expired, 1);
	smp_gateway_wake();
}

static int link_start(struct link *l, size_t i)
{
	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
		BT_DATA(BT_DATA_NAME_COMPLETE, l->name, strcspn(l->name, "@")),
	};
	const struct bt_data sd[] = {
		BT_DATA_BYTES(BT_DATA_UUID128_ALL, SMP_BT_SVC_UUID_VAL),
	};
	struct bt_le_adv_param param = {
		.options = BT_LE_ADV_OPT_CONN,
		.interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
		.interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
	};
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	char text[BT_ADDR_LE_STR_LEN];
	size_t count = ARRAY_SIZE(addrs);
	int ret = 0;

	k_timer_init(&l->idle, idle_expired, NULL);
	bt_id_get(addrs, &count);
	/* Identity 0 is there already. The others follow from it: the same address at every boot,
	 * or a tool's bond to this device would not last.
	 */
	if (i > 0) {
		bt_addr_le_t addr = addrs[0];

		addr.a.val[0] += i;
		ret = bt_id_create(&addr, NULL);
		if (ret < 0) {
			return ret;
		}
		l->id = ret;
		addrs[l->id] = addr;
	}
	param.id = l->id;
	ret = bt_le_ext_adv_create(&param, NULL, &l->adv);
	ret = ret ? ret : bt_le_ext_adv_set_data(l->adv, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (ret == 0) {
		bt_addr_le_to_str(&addrs[l->id], text, sizeof(text));
		LOG_INF("%s for 0x%02x at %s", l->name, l->addr, text);
	}
	return ret;
}

static void bt_start(void)
{
	int ret = bt_enable(NULL);

	if (ret != 0) {
		LOG_ERR("Bluetooth: %d", ret);
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
		ret = link_start(&links[i], i);
		if (ret != 0) {
			LOG_ERR("%s: %d", links[i].name, ret);
		}
	}
	advertise();
}

static void bt_serve(void)
{
	if (atomic_cas(&advertise_again, 1, 0)) {
		advertise();
	}
	for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
		struct link *l = &links[i];
		struct bt_conn *conn;
		struct net_buf *req;

		K_SPINLOCK(&lock) {
			req = l->ready;
			l->ready = NULL;
		}
		if (req != NULL) {
			/* Stopped until the response: a quiet tool counts, not a slow MCU */
			k_timer_stop(&l->idle);
			smp_gateway_forward(l->addr, req, respond, l);
			atomic_clear(&l->idle_expired);
			k_timer_start(&l->idle, IDLE_TIMEOUT, K_NO_WAIT);
		} else if (atomic_cas(&l->idle_expired, 1, 0)) {
			conn = conn_get(l);
			if (conn != NULL) {
				LOG_WRN("%s: no SMP for %d ms, disconnecting", l->name,
					CONFIG_TWIXT_SMP_GATEWAY_BT_IDLE_MS);
				bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
				bt_conn_unref(conn);
			}
		}
	}
}

SMP_GATEWAY_FRONTEND_DEFINE(bt_frontend, bt_start, bt_serve);
