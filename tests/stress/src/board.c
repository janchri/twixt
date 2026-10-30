/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A board: its shell is on a UART over the bus to the gateway (board.overlay), where the test
 * starts and reads "twixt stress". The prompt is its name: mcu-a@10:~$
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell_uart.h>

#include <twixt/twixt.h>

LOG_MODULE_REGISTER(board, LOG_LEVEL_INF);

int main(void)
{
	static char prompt[32];

	snprintf(prompt, sizeof(prompt), "%s:~$ ", twixt_self_name());
	shell_prompt_change(shell_backend_uart_get_ptr(), prompt);

	LOG_INF("%s at 0x%02x, shell on the bus", twixt_self_name(), twixt_self_addr());
	return 0;
}
