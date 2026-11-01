/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <twixt/link.h>

LOG_MODULE_REGISTER(twixt_link, CONFIG_TWIXT_LINK_LOG_LEVEL);

#include "target_restore.h"

void twixt_link_get_stats(const struct twixt_link *link, struct twixt_link_stats *stats)
{
	*stats = link->stats;
	stats->target_restore_failures = target_restore_failures();
	stats->target_down = target_restore_down();
}

static uint8_t next_rsp_byte(struct twixt_link *link)
{
	return link->rsp_pos < link->rsp_len ? link->rsp[link->rsp_pos++] : 0xff;
}

static int write_requested(struct i2c_target_config *cfg)
{
	struct twixt_link *link = CONTAINER_OF(cfg, struct twixt_link, target);

	link->req_len = 0;
	return 0;
}

static int stop(struct i2c_target_config *cfg)
{
	struct twixt_link *link = CONTAINER_OF(cfg, struct twixt_link, target);

	link->req_len = 0;
	link->req_overflow = false;
	return 0;
}

static int write_received(struct i2c_target_config *cfg, uint8_t val)
{
	struct twixt_link *link = CONTAINER_OF(cfg, struct twixt_link, target);

	if (link->req_overflow || link->req_len >= sizeof(link->req)) {
		link->req_overflow = true;
		return -ENOMEM;
	}
	link->req[link->req_len++] = val;
	return 0;
}

static int read_requested(struct i2c_target_config *cfg, uint8_t *val)
{
	struct twixt_link *link = CONTAINER_OF(cfg, struct twixt_link, target);

	link->rsp_len = link->serve(link->req, link->req_overflow ? 0 : link->req_len, link->rsp);
	link->rsp_pos = 0;
	link->req_len = 0;
	link->stats.served++;
	*val = next_rsp_byte(link);
	return 0;
}

static int read_processed(struct i2c_target_config *cfg, uint8_t *val)
{
	struct twixt_link *link = CONTAINER_OF(cfg, struct twixt_link, target);

	*val = next_rsp_byte(link);
	return 0;
}

#ifdef CONFIG_I2C_TARGET_BUFFER_MODE
static void buf_write_received(struct i2c_target_config *cfg, uint8_t *buf, uint32_t len)
{
	struct twixt_link *link = CONTAINER_OF(cfg, struct twixt_link, target);

	link->req_len = len <= sizeof(link->req) ? len : 0;
	memcpy(link->req, buf, link->req_len);
}

static int buf_read_requested(struct i2c_target_config *cfg, uint8_t **buf, uint32_t *len)
{
	struct twixt_link *link = CONTAINER_OF(cfg, struct twixt_link, target);

	link->rsp_len = link->serve(link->req, link->req_len, link->rsp);
	memset(&link->rsp[link->rsp_len], 0xff, sizeof(link->rsp) - link->rsp_len);
	link->req_len = 0;
	link->stats.served++;
	*buf = link->rsp;
	*len = sizeof(link->rsp);
	return 0;
}
#endif

static const struct i2c_target_callbacks target_cb = {
	.write_requested = write_requested,
	.write_received = write_received,
	.read_requested = read_requested,
	.read_processed = read_processed,
#ifdef CONFIG_I2C_TARGET_BUFFER_MODE
	.buf_write_received = buf_write_received,
	.buf_read_requested = buf_read_requested,
#endif
	.stop = stop,
};

int twixt_link_init(struct twixt_link *link, const struct device *i2c, uint16_t addr,
		    twixt_link_serve_cb serve)
{
	int ret;

	if (!device_is_ready(i2c)) {
		LOG_ERR("%s not ready", i2c->name);
		return -ENODEV;
	}
	link->i2c = i2c;
	link->serve = serve;
	k_mutex_init(&link->lock);
	link->target.address = addr;
	link->target.callbacks = &target_cb;
	ret = i2c_target_register(i2c, &link->target);
	if (ret) {
		LOG_ERR("%s: cannot serve at 0x%02x: %d", i2c->name, addr, ret);
	}
	return ret;
}

static void transfer_self(struct twixt_link *link, const uint8_t *req, size_t req_len, uint8_t *rsp,
			  size_t rsp_len)
{
	uint8_t out[TWIXT_LINK_MSG_MAX];
	size_t len = link->serve(req, req_len, out);

	memset(&out[len], 0xff, sizeof(out) - len);
	memcpy(rsp, out, rsp_len);
}

int twixt_link_transfer(struct twixt_link *link, uint16_t addr, const void *req, size_t req_len,
			void *rsp, size_t rsp_len)
{
	k_timepoint_t end = sys_timepoint_calc(K_MSEC(CONFIG_TWIXT_LINK_RETRY_MS));
	k_timepoint_t nack_end = sys_timepoint_calc(K_MSEC(CONFIG_TWIXT_LINK_NACK_RETRY_MS));

	if (req_len > TWIXT_LINK_MSG_MAX || rsp_len > TWIXT_LINK_MSG_MAX) {
		return -EINVAL;
	}
	if (addr == link->target.address) {
		transfer_self(link, req, req_len, rsp, rsp_len);
		return 0;
	}

	for (uint32_t i = 1;; i++) {
		bool nack;
		int ret;

		/* re-registering must not hit another thread's transfer */
		k_mutex_lock(&link->lock, K_FOREVER);
		ret = i2c_write_read(link->i2c, addr, req, req_len, rsp, rsp_len);
		if (IS_ENABLED(CONFIG_I2C_MM_TARGET_REREGISTER)) {
			target_restore_report(target_reregister(link->i2c, &link->target));
		}
		nack = ret == -ENXIO;
		if (ret == 0) {
			link->stats.sent++;
		} else if (ret == -EAGAIN || ret == -EBUSY || nack) {
			link->stats.retries++;
		}
		k_mutex_unlock(&link->lock);

		if (nack) {
			if (sys_timepoint_expired(nack_end)) {
				return -ENXIO;
			}
			k_sleep(K_MSEC(1));
			continue;
		}
		if (ret != -EAGAIN && ret != -EBUSY) {
			return ret;
		}
		if (sys_timepoint_expired(end)) {
			LOG_WRN("0x%02x: request to 0x%02x: %s after %u attempts",
				link->target.address, addr,
				ret == -EAGAIN ? "lost arbitration" : "bus busy", i);
			return ret;
		}
		LOG_DBG("0x%02x: request to 0x%02x attempt %u: %s, backing off",
			link->target.address, addr, i,
			ret == -EAGAIN ? "lost arbitration" : "bus busy");
		/* out of the winner's START window, then wait for its STOP */
		k_sleep(K_MSEC(2 * MIN(i, 4)));
	}
}
