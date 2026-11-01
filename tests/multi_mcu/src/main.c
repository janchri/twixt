/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two MCUs use each of twixt's services on the other, again and again, and log what they got:
 * pytest/test_multi_mcu.py reads the logs. A service of the test adds two numbers. The settings:
 *
 *   mcu/name       read-only   the devicetree node of the MCU
 *   mcu/greeting   read/write  any text, kept in flash
 *
 * pico also sends raw transfers to a counting service on nrf: a request runs once or not at all,
 * and a read without one gets EBADMSG. Zephyr's nRF TWIS driver does not report STOP, and its
 * "write done" is the same after a STOP and a repeated START: on the nRF, twixt,i2c-mm-gpio tells
 * them apart by the lines after it, and only real boards prove that.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/byteorder.h>

#include <twixt/twixt.h>
#include <twixt/service/echo.h>
#include <twixt/service/ping.h>
#include <twixt/service/settings.h>
#include <twixt/service/smp.h>

LOG_MODULE_REGISTER(multi_mcu, LOG_LEVEL_INF);

#define ADD_SERVICE   TWIXT_SERVICE_APP
#define COUNT_SERVICE (TWIXT_SERVICE_APP + 1)
#define BUS           DEVICE_DT_GET(DT_BUS(TWIXT_SELF))
#define RAW_TRIES     50
#define PROTOCOL_RUNS 100
#define SMP_TEXT      "hello over the bus"
#define SMP_HDR_LEN   8

#define PICO DT_REG_ADDR(DT_NODELABEL(mcu_pico))
#define NRF  DT_REG_ADDR(DT_NODELABEL(mcu_nrf))
#define PEER (twixt_self_addr() == PICO ? NRF : PICO)

static int add(const struct twixt_req *req, uint8_t *rsp_data)
{
	if (req->len != 8) {
		return -EINVAL;
	}
	sys_put_le32(sys_get_le32(&req->data[0]) + sys_get_le32(&req->data[4]), rsp_data);
	return 4;
}

TWIXT_SERVICE_DEFINE(add_service, ADD_SERVICE, add);

static atomic_t runs;

static int count(const struct twixt_req *req, uint8_t *rsp)
{
	sys_put_le32(atomic_inc(&runs) + 1, rsp);
	return 4;
}

TWIXT_SERVICE_DEFINE(count_service, COUNT_SERVICE, count);

static struct k_spinlock lock;
static char greeting[TWIXT_SETTINGS_VALUE_MAX];
static size_t greeting_len;

static int mcu_get(const char *key, char *val, int val_len_max)
{
	int len = -ENOENT;

	if (settings_name_steq(key, "name", NULL)) {
		len = strlen(twixt_self_name());
		memcpy(val, twixt_self_name(), MIN(len, val_len_max));
	} else if (settings_name_steq(key, "greeting", NULL)) {
		K_SPINLOCK(&lock) {
			len = greeting_len;
			memcpy(val, greeting, MIN(len, val_len_max));
		}
	}
	return len;
}

static int mcu_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	char val[sizeof(greeting)];
	ssize_t ret;

	if (settings_name_steq(key, "name", NULL)) {
		return -EACCES;
	}
	if (!settings_name_steq(key, "greeting", NULL)) {
		return -ENOENT;
	}
	if (len > sizeof(val)) {
		return -EINVAL;
	}
	ret = read_cb(cb_arg, val, len);
	if (ret < 0) {
		return ret;
	}
	K_SPINLOCK(&lock) {
		memcpy(greeting, val, len);
		greeting_len = len;
	}
	return 0;
}

static int mcu_export(int (*export_func)(const char *name, const void *val, size_t val_len))
{
	char val[sizeof(greeting)];
	size_t len;

	K_SPINLOCK(&lock) {
		len = greeting_len;
		memcpy(val, greeting, len);
	}
	return len > 0 ? export_func("mcu/greeting", val, len) : 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(mcu, "mcu", mcu_get, mcu_set, NULL, mcu_export);

static int check_echo(void)
{
	char text[TWIXT_ECHO_MAX];
	char back[TWIXT_ECHO_MAX];
	int len = snprintf(text, sizeof(text), "hello from %s", twixt_self_name());
	int ret = twixt_echo(PEER, text, len, back);

	if (ret == 0 && memcmp(text, back, len) != 0) {
		return -EBADMSG;
	}
	if (ret == 0) {
		LOG_INF("echo from 0x%02x: \"%.*s\"", PEER, len, back);
	}
	return ret;
}

static int check_add(void)
{
	uint8_t req[8];
	uint8_t rsp[4];
	int32_t a = twixt_self_addr() == PICO ? 2 : 40;
	int32_t b = twixt_self_addr() == PICO ? 3 : 2;
	int ret;

	sys_put_le32(a, &req[0]);
	sys_put_le32(b, &req[4]);
	ret = twixt_request(PEER, ADD_SERVICE, req, sizeof(req), rsp, sizeof(rsp));
	if (ret == 0) {
		LOG_INF("0x%02x adds %d + %d = %d", PEER, a, b, (int32_t)sys_get_le32(rsp));
	}
	return ret;
}

static int check_settings(void)
{
	char val[TWIXT_SETTINGS_VALUE_MAX];
	char hi[TWIXT_SETTINGS_VALUE_MAX];
	int len = twixt_settings_get(PEER, "mcu/name", val, sizeof(val));
	int ret;

	if (len < 0) {
		return len;
	}
	LOG_INF("0x%02x is %.*s", PEER, MIN(len, (int)sizeof(val)), val);

	len = snprintf(hi, sizeof(hi), "hi from %s", twixt_self_name());
	ret = twixt_settings_set(PEER, "mcu/greeting", hi, MIN(len, (int)sizeof(hi) - 1));
	if (ret) {
		return ret;
	}
	len = twixt_settings_get(PEER, "mcu/greeting", val, sizeof(val));
	if (len < 0) {
		return len;
	}
	LOG_INF("0x%02x mcu/greeting = \"%.*s\"", PEER, MIN(len, (int)sizeof(val)), val);

	ret = twixt_settings_set(PEER, "mcu/name", "x", 1);
	LOG_INF("0x%02x mcu/name is read-only: %d", PEER, ret);
	return 0;
}

/* The other MCU sends as well: wait out a bus it holds, as the link does */
static int raw(struct i2c_msg *msgs, uint8_t num)
{
	int ret;

	for (int i = 0; i < RAW_TRIES; i++) {
		ret = i2c_transfer(BUS, msgs, num, PEER);
		if (ret != -ENXIO && ret != -EAGAIN && ret != -EBUSY) {
			break;
		}
		k_msleep(1);
	}
	return ret;
}

static int raw_write_read(const uint8_t *req, size_t req_len, uint8_t *rsp, size_t rsp_len)
{
	struct i2c_msg msgs[] = {
		{.buf = (uint8_t *)req, .len = req_len, .flags = I2C_MSG_WRITE},
		{.buf = rsp,
		 .len = rsp_len,
		 .flags = I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP},
	};

	return raw(msgs, ARRAY_SIZE(msgs));
}

static int raw_write(const uint8_t *req, size_t len)
{
	struct i2c_msg msg = {
		.buf = (uint8_t *)req, .len = len, .flags = I2C_MSG_WRITE | I2C_MSG_STOP};

	return raw(&msg, 1);
}

static int raw_read(uint8_t *rsp, size_t len)
{
	struct i2c_msg msg = {.buf = rsp, .len = len, .flags = I2C_MSG_READ | I2C_MSG_STOP};

	return raw(&msg, 1);
}

static int protocol_round(void)
{
	const uint8_t req[] = {COUNT_SERVICE, twixt_self_addr(), 0};
	uint8_t rsp[5];
	uint32_t before;
	int ret = raw_write_read(req, sizeof(req), rsp, sizeof(rsp));

	if (ret != 0 || rsp[0] != 0) {
		return -EIO;
	}
	before = sys_get_le32(&rsp[1]);

	ret = raw_write(req, sizeof(req));
	if (ret != 0) {
		return ret;
	}
	ret = raw_read(rsp, sizeof(rsp));
	if (ret != 0 || rsp[0] != EBADMSG) {
		LOG_ERR("abandoned request: ret %d status %u", ret, rsp[0]);
		return -EIO;
	}

	ret = raw_write_read(req, sizeof(req), rsp, sizeof(rsp));
	if (ret != 0 || rsp[0] != 0 || sys_get_le32(&rsp[1]) != before + 1) {
		return -EIO;
	}
	ret = raw_read(rsp, sizeof(rsp));
	return ret != 0 || rsp[0] != EBADMSG ? -EIO : 0;
}

static int check_protocol(void)
{
	int ret = 0;

	if (twixt_self_addr() != PICO) {
		return 0;
	}
	for (int i = 0; ret == 0 && i < PROTOCOL_RUNS; i++) {
		ret = protocol_round();
	}
	if (ret == 0) {
		LOG_INF("protocol: %d rounds", PROTOCOL_RUNS);
	}
	return ret;
}

/* os echo {"d": SMP_TEXT}: SMP v2, write, group 0, command 0 */
static size_t smp_echo_request(uint8_t *pkt)
{
	static const uint8_t cbor[] = {0xa1, 0x61, 'd', 0x60 + sizeof(SMP_TEXT) - 1};
	size_t len = sizeof(cbor) + sizeof(SMP_TEXT) - 1;

	memset(pkt, 0, SMP_HDR_LEN);
	pkt[0] = (1 << 3) | 2;
	sys_put_be16(len, &pkt[2]);
	memcpy(&pkt[SMP_HDR_LEN], cbor, sizeof(cbor));
	memcpy(&pkt[SMP_HDR_LEN + sizeof(cbor)], SMP_TEXT, sizeof(SMP_TEXT) - 1);
	return SMP_HDR_LEN + len;
}

/* MCUmgr encodes the response map with indefinite length: the text is inside */
static bool smp_contains(const struct net_buf *nb, const char *text)
{
	for (size_t i = SMP_HDR_LEN; i + strlen(text) <= nb->len; i++) {
		if (memcmp(&nb->data[i], text, strlen(text)) == 0) {
			return true;
		}
	}
	return false;
}

static int check_smp(void)
{
	uint8_t pkt[SMP_HDR_LEN + 4 + sizeof(SMP_TEXT)];
	struct net_buf *nb;
	uint8_t src;
	int ret = twixt_smp_send(PEER, pkt, smp_echo_request(pkt));

	if (ret) {
		return ret;
	}
	nb = twixt_smp_rsp_get(&src, K_SECONDS(5));
	if (nb == NULL) {
		return -ETIMEDOUT;
	}
	ret = src == PEER && smp_contains(nb, SMP_TEXT) ? 0 : -EBADMSG;
	smp_packet_free(nb);
	if (ret == 0) {
		LOG_INF("MCUmgr echo from 0x%02x: %s", src, SMP_TEXT);
	}
	return ret;
}

int main(void)
{
	struct twixt_ping_info info;
	int ret = settings_subsys_init();

	ret = ret ? ret : settings_load();
	if (ret) {
		LOG_ERR("cannot load the settings: %d", ret);
		return 0;
	}
	LOG_INF("%s at 0x%02x, greeting from flash: \"%.*s\"", twixt_self_name(), twixt_self_addr(),
		(int)greeting_len, greeting);

	for (;; k_sleep(K_SECONDS(2))) {
		ret = twixt_ping_wait(PEER, &info, K_SECONDS(5));
		if (ret == 0) {
			LOG_INF("0x%02x answers: protocol %u, messages up to %u bytes", PEER,
				info.version, info.msg_max);
		}
		ret = ret ? ret : check_echo();
		ret = ret ? ret : check_add();
		ret = ret ? ret : check_settings();
		ret = ret ? ret : check_protocol();
		ret = ret ? ret : check_smp();
		if (ret) {
			LOG_ERR("0x%02x: %d", PEER, ret);
		}
	}
	return 0;
}
