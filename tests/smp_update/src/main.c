/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The update is all in twixt and Zephyr, see README.rst.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <twixt/twixt.h>

LOG_MODULE_REGISTER(smp_update, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("%s at 0x%02x", twixt_self_name(), twixt_self_addr());
	return 0;
}
