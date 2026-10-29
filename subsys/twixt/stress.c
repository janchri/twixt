/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include <twixt/twixt.h>
#include <twixt/echo.h>

#define TARGETS_MAX 4
#define ERRNOS_MAX  4

struct target {
	uint8_t addr;
	uint32_t ok;
	uint32_t wrong;
	uint32_t err;
	struct {
		int err;
		uint32_t count;
	} errnos[ERRNOS_MAX];
	uint32_t other;
	uint32_t max_us;
	uint64_t sum_us;
	int64_t first_fail_ms;
};

static struct target targets[TARGETS_MAX];
static size_t num_targets;
static uint32_t interval_ms;
static int64_t start_ms;
static k_timepoint_t deadline;
static bool started;
static int64_t stop_ms;
static atomic_t running;
static struct k_spinlock lock;

static K_THREAD_STACK_DEFINE(stack, CONFIG_TWIXT_STRESS_STACK_SIZE);
static struct k_thread thread;

static void count_err(struct target *t, int err)
{
	t->err++;
	for (int i = 0; i < ERRNOS_MAX; i++) {
		if (t->errnos[i].count == 0 || t->errnos[i].err == err) {
			t->errnos[i].err = err;
			t->errnos[i].count++;
			return;
		}
	}
	t->other++;
}

static void run(void *p1, void *p2, void *p3)
{
	uint8_t req[TWIXT_ECHO_MAX];
	uint8_t rsp[TWIXT_ECHO_MAX];

	for (uint32_t seq = 0; atomic_get(&running) && !sys_timepoint_expired(deadline); seq++) {
		struct target *t = &targets[seq % num_targets];
		size_t len = (seq / num_targets) % (TWIXT_ECHO_MAX + 1);

		for (size_t i = 0; i < len; i++) {
			req[i] = seq * 31 + i * 7 + twixt_self_addr();
		}
		memset(rsp, 0, sizeof(rsp));

		uint32_t t0 = k_cycle_get_32();
		int ret = twixt_echo(t->addr, req, len, rsp);
		uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

		K_SPINLOCK(&lock) {
			if (ret) {
				count_err(t, ret);
			} else if (memcmp(req, rsp, len) != 0) {
				t->wrong++;
			} else {
				t->ok++;
				t->sum_us += us;
				t->max_us = MAX(t->max_us, us);
			}
			if ((ret || t->wrong) && t->first_fail_ms < 0) {
				t->first_fail_ms = k_uptime_get();
			}
		}
		if (interval_ms > 0) {
			k_msleep(interval_ms);
		}
	}
	K_SPINLOCK(&lock) {
		stop_ms = k_uptime_get();
	}
	atomic_set(&running, 0);
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	unsigned long ms = shell_strtoul(argv[1], 0, &err);
	unsigned long secs = err ? 0 : shell_strtoul(argv[2], 0, &err);

	if (err) {
		shell_error(sh, "not a number: %s %s", argv[1], argv[2]);
		return -EINVAL;
	}
	if (argc - 3 > TARGETS_MAX) {
		shell_error(sh, "at most %d addresses", TARGETS_MAX);
		return -EINVAL;
	}
	if (atomic_get(&running)) {
		shell_error(sh, "already running");
		return -EBUSY;
	}

	if (started) {
		k_thread_join(&thread, K_FOREVER);
	}

	memset(targets, 0, sizeof(targets));
	for (size_t i = 3; i < argc; i++) {
		if (twixt_shell_parse_addr(sh, argv[i], &targets[i - 3].addr)) {
			return -EINVAL;
		}
		targets[i - 3].first_fail_ms = -1;
	}
	num_targets = argc - 3;
	interval_ms = ms;
	deadline = sys_timepoint_calc(secs ? K_SECONDS(secs) : K_FOREVER);
	start_ms = k_uptime_get();
	stop_ms = 0;
	started = true;
	atomic_set(&running, 1);
	k_thread_create(&thread, stack, K_THREAD_STACK_SIZEOF(stack), run, NULL, NULL, NULL,
			K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&thread, "twixt_stress");
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	atomic_set(&running, 0);
	if (started) {
		k_thread_join(&thread, K_FOREVER);
	}
	return 0;
}

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	struct target t[TARGETS_MAX];
	size_t n;
	int64_t end;

	K_SPINLOCK(&lock) {
		memcpy(t, targets, sizeof(t));
		n = num_targets;
		end = stop_ms ? stop_ms : k_uptime_get();
	}
	shell_print(sh, "%s at 0x%02x: %s, %lld ms, interval %u ms", twixt_self_name(),
		    twixt_self_addr(), atomic_get(&running) ? "running" : "stopped",
		    n ? end - start_ms : 0, interval_ms);
	for (size_t i = 0; i < n; i++) {
		char errs[64] = "";
		int pos = 0;

		for (int j = 0; j < ERRNOS_MAX && t[i].errnos[j].count; j++) {
			pos += snprintf(&errs[pos], sizeof(errs) - pos, " %d:%u", t[i].errnos[j].err,
					t[i].errnos[j].count);
		}
		if (t[i].other) {
			snprintf(&errs[pos], sizeof(errs) - pos, " other:%u", t[i].other);
		}
		shell_print(sh, "  0x%02x: ok %u, wrong %u, err %u%s%s, max %u us, avg %u us",
			    t[i].addr, t[i].ok, t[i].wrong, t[i].err, t[i].err ? " (" : "",
			    t[i].err ? errs + 1 : "", t[i].max_us,
			    t[i].ok ? (uint32_t)(t[i].sum_us / t[i].ok) : 0);
		if (t[i].err || t[i].wrong) {
			shell_print(sh, "        first failure at %lld ms", t[i].first_fail_ms - start_ms);
		}
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(stress_cmds,
	SHELL_CMD_ARG(start, NULL,
		      "<interval_ms> <seconds> <addr>...: echo requests round-robin; 0 s: until stop",
		      cmd_start, 4, TARGETS_MAX - 1),
	SHELL_CMD_ARG(stop, NULL, "stop sending", cmd_stop, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((twixt), stress, &stress_cmds, "echo load: start, stop; alone: the counts",
		 cmd_show, 1, 0);
