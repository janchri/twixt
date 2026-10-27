/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <twixt/twixt.h>

LOG_MODULE_REGISTER(twixt, CONFIG_TWIXT_LOG_LEVEL);

BUILD_ASSERT(DT_NODE_EXISTS(TWIXT_SELF), "twixt needs /chosen twixt,self");

static struct i2c_mm link;

const char *twixt_self_name(void)
{
	return DT_NODE_FULL_NAME(TWIXT_SELF);
}

uint8_t twixt_self_addr(void)
{
	return DT_REG_ADDR(TWIXT_SELF);
}

void twixt_get_stats(struct i2c_mm_stats *stats)
{
	i2c_mm_get_stats(&link, stats);
}

#define TWIXT_BADMSG_TRIES 3

static size_t serve(const uint8_t *req, size_t len, uint8_t *rsp)
{
	int ret = -EBADMSG;

	if (len >= TWIXT_REQ_HDR_LEN) {
		const struct twixt_req r = {
			.src = req[1],
			.data = &req[TWIXT_REQ_HDR_LEN],
			.len = len - TWIXT_REQ_HDR_LEN,
		};

		ret = -ENOTSUP;
		STRUCT_SECTION_FOREACH(twixt_service, s) {
			if (s->id == req[0]) {
				ret = s->handler(&r, &rsp[1]);
				if (ret == -EIO || ret == -EAGAIN || ret == -EBUSY ||
				    ret == -EBADMSG) {
					LOG_ERR("service 0x%02x: %d is for the bus", s->id, ret);
					ret = -EFAULT;
				}
				break;
			}
		}
	}
	/* errno without its sign: Zephyr's errno values go past 127 (ENOTSUP is 134) */
	rsp[0] = ret < 0 ? MIN(-ret, UINT8_MAX) : 0;
	return 1 + MAX(ret, 0);
}

int twixt_request(uint8_t addr, uint8_t service, const void *data, size_t len, void *rsp_data,
		  size_t rsp_len)
{
	uint8_t req[TWIXT_MSG_MAX];
	uint8_t in[TWIXT_MSG_MAX];
	int ret;

	if (len > TWIXT_REQ_DATA_MAX || rsp_len > TWIXT_RSP_DATA_MAX) {
		return -EINVAL;
	}
	req[0] = service;
	req[1] = twixt_self_addr();
	req[2] = 0;
	if (len > 0) {
		memcpy(&req[TWIXT_REQ_HDR_LEN], data, len);
	}

	for (int i = 1;; i++) {
		ret = i2c_mm_transfer(&link, addr, req, TWIXT_REQ_HDR_LEN + len, in, 1 + rsp_len);
		if (ret) {
			return ret;
		}
		if (in[0] != EBADMSG || i == TWIXT_BADMSG_TRIES) {
			break;
		}
		LOG_WRN("0x%02x: request to 0x%02x attempt %d: incomplete there, again",
			twixt_self_addr(), addr, i);
	}
	if (in[0] != 0) {
		return -in[0];
	}
	if (rsp_len > 0) {
		memcpy(rsp_data, &in[1], rsp_len);
	}
	return 0;
}

static bool ids_unique(void)
{
	bool unique = true;

	STRUCT_SECTION_FOREACH(twixt_service, a) {
		STRUCT_SECTION_FOREACH(twixt_service, b) {
			if (a < b && a->id == b->id) {
				LOG_ERR("two services with id 0x%02x", a->id);
				unique = false;
			}
		}
	}
	return unique;
}

static int twixt_init(void)
{
	if (!ids_unique()) {
		return -EEXIST;
	}
	return i2c_mm_init(&link, DEVICE_DT_GET(DT_BUS(TWIXT_SELF)), twixt_self_addr(), serve);
}

SYS_INIT(twixt_init, POST_KERNEL, CONFIG_TWIXT_INIT_PRIORITY);

#ifdef CONFIG_TWIXT_SHELL

int twixt_shell_parse_addr(const struct shell *sh, const char *arg, uint8_t *addr)
{
	int err = 0;
	unsigned long v = shell_strtoul(arg, 0, &err);

	if (err || v > 0x7f) {
		shell_error(sh, "not an I2C address: %s", arg);
		return -EINVAL;
	}
	*addr = v;
	return 0;
}

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct i2c_mm_stats s;

	i2c_mm_get_stats(&link, &s);
	shell_print(sh, "%s at 0x%02x: sent %u, served %u, retries %u", twixt_self_name(),
		    twixt_self_addr(), s.sent, s.served, s.retries);
	return 0;
}

SHELL_SUBCMD_SET_CREATE(twixt_cmds, (twixt));
SHELL_SUBCMD_ADD((twixt), stats, NULL, "requests of this MCU", cmd_stats, 1, 0);
SHELL_CMD_REGISTER(twixt, &twixt_cmds, "The MCUs on the bus", NULL);

#endif /* CONFIG_TWIXT_SHELL */
