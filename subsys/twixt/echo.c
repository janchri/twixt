/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/shell/shell.h>

#include <twixt/twixt.h>
#include <twixt/echo.h>

static int echo(const struct twixt_req *req, uint8_t *rsp_data)
{
	memcpy(rsp_data, req->data, req->len);
	return req->len;
}

TWIXT_SERVICE_DEFINE(twixt_echo_service, TWIXT_SERVICE_ECHO, echo);

int twixt_echo(uint8_t addr, const void *data, size_t len, void *back)
{
	if (len > TWIXT_ECHO_MAX) {
		return -EINVAL;
	}
	return twixt_request(addr, TWIXT_SERVICE_ECHO, data, len, back, len);
}

#ifdef CONFIG_TWIXT_SHELL

static int cmd_echo(const struct shell *sh, size_t argc, char **argv)
{
	char back[TWIXT_ECHO_MAX + 1] = "";
	size_t len = strlen(argv[2]);
	uint8_t addr;
	int ret;

	if (twixt_shell_parse_addr(sh, argv[1], &addr)) {
		return -EINVAL;
	}
	if (len > TWIXT_ECHO_MAX) {
		shell_error(sh, "at most %d characters", TWIXT_ECHO_MAX);
		return -EINVAL;
	}
	ret = twixt_echo(addr, argv[2], len, back);
	if (ret) {
		shell_error(sh, "0x%02x: %d", addr, ret);
		return ret;
	}
	shell_print(sh, "0x%02x: %s%s", addr, back,
		    memcmp(back, argv[2], len) ? ": differs from what was sent" : "");
	return 0;
}

SHELL_SUBCMD_ADD((twixt), echo, NULL, "<addr> <text>: the MCU at <addr> sends <text> back",
		 cmd_echo, 3, 0);

#endif /* CONFIG_TWIXT_SHELL */
