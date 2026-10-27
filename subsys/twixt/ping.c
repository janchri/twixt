/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/shell/shell.h>

#include <twixt/twixt.h>
#include <twixt/ping.h>

static int ping(const struct twixt_req *req, uint8_t *rsp_data)
{
	ARG_UNUSED(req);

	rsp_data[0] = TWIXT_PROTOCOL_VERSION;
	rsp_data[1] = TWIXT_MSG_MAX;
	return 2;
}

TWIXT_SERVICE_DEFINE(twixt_ping_service, TWIXT_SERVICE_PING, ping);

int twixt_ping(uint8_t addr, struct twixt_ping_info *info)
{
	uint8_t rsp[2];
	int ret = twixt_request(addr, TWIXT_SERVICE_PING, NULL, 0, rsp, sizeof(rsp));

	if (ret) {
		return ret;
	}
	info->version = rsp[0];
	info->msg_max = rsp[1];
	return info->version == TWIXT_PROTOCOL_VERSION && info->msg_max == TWIXT_MSG_MAX ? 0
											 : -EPROTO;
}

int twixt_ping_wait(uint8_t addr, struct twixt_ping_info *info, k_timeout_t timeout)
{
	k_timepoint_t end = sys_timepoint_calc(timeout);
	int ret;

	while ((ret = twixt_ping(addr, info)) == -EIO && !sys_timepoint_expired(end)) {
		k_msleep(100);
	}
	return ret;
}

#ifdef CONFIG_TWIXT_SHELL

static int cmd_ping(const struct shell *sh, size_t argc, char **argv)
{
	struct twixt_ping_info info;
	uint8_t addr;
	int ret;

	if (twixt_shell_parse_addr(sh, argv[1], &addr)) {
		return -EINVAL;
	}
	ret = twixt_ping(addr, &info);
	if (ret == 0 || ret == -EPROTO) {
		shell_print(sh, "0x%02x: protocol %u, messages up to %u bytes%s", addr,
			    info.version, info.msg_max, ret ? ": does not match this MCU" : "");
	} else {
		shell_error(sh, "0x%02x: %d", addr, ret);
	}
	return ret;
}

SHELL_SUBCMD_ADD((twixt), ping, NULL, "<addr>: protocol and limits of the MCU at <addr>", cmd_ping,
		 2, 0);

#endif /* CONFIG_TWIXT_SHELL */
