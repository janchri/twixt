/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/i2c/i2c_mm.h>

LOG_MODULE_REGISTER(i2c_mm, CONFIG_I2C_MM_LOG_LEVEL);

#include "target_reregister.h"

void i2c_mm_get_stats(const struct i2c_mm *mm, struct i2c_mm_stats *stats)
{
	*stats = mm->stats;
}

static uint8_t next_rsp_byte(struct i2c_mm *mm)
{
	return mm->rsp_pos < mm->rsp_len ? mm->rsp[mm->rsp_pos++] : 0xff;
}

static int forget_request(struct i2c_target_config *cfg)
{
	struct i2c_mm *mm = CONTAINER_OF(cfg, struct i2c_mm, target);

	mm->req_len = 0;
	return 0;
}

static int write_received(struct i2c_target_config *cfg, uint8_t val)
{
	struct i2c_mm *mm = CONTAINER_OF(cfg, struct i2c_mm, target);

	if (mm->req_len >= sizeof(mm->req)) {
		return -ENOMEM;
	}
	mm->req[mm->req_len++] = val;
	return 0;
}

static int read_requested(struct i2c_target_config *cfg, uint8_t *val)
{
	struct i2c_mm *mm = CONTAINER_OF(cfg, struct i2c_mm, target);

	mm->rsp_len = mm->serve(mm->req, mm->req_len, mm->rsp);
	mm->rsp_pos = 0;
	mm->req_len = 0;
	mm->stats.served++;
	*val = next_rsp_byte(mm);
	return 0;
}

static int read_processed(struct i2c_target_config *cfg, uint8_t *val)
{
	struct i2c_mm *mm = CONTAINER_OF(cfg, struct i2c_mm, target);

	*val = next_rsp_byte(mm);
	return 0;
}

#ifdef CONFIG_I2C_TARGET_BUFFER_MODE
static void buf_write_received(struct i2c_target_config *cfg, uint8_t *buf, uint32_t len)
{
	struct i2c_mm *mm = CONTAINER_OF(cfg, struct i2c_mm, target);

	mm->req_len = MIN(len, sizeof(mm->req));
	memcpy(mm->req, buf, mm->req_len);
}

static int buf_read_requested(struct i2c_target_config *cfg, uint8_t **buf, uint32_t *len)
{
	struct i2c_mm *mm = CONTAINER_OF(cfg, struct i2c_mm, target);

	mm->rsp_len = mm->serve(mm->req, mm->req_len, mm->rsp);
	memset(&mm->rsp[mm->rsp_len], 0xff, sizeof(mm->rsp) - mm->rsp_len);
	mm->req_len = 0;
	mm->stats.served++;
	*buf = mm->rsp;
	*len = sizeof(mm->rsp);
	return 0;
}
#endif

static const struct i2c_target_callbacks target_cb = {
	.write_requested = forget_request,
	.write_received = write_received,
	.read_requested = read_requested,
	.read_processed = read_processed,
#ifdef CONFIG_I2C_TARGET_BUFFER_MODE
	.buf_write_received = buf_write_received,
	.buf_read_requested = buf_read_requested,
#endif
	.stop = forget_request,
};

int i2c_mm_init(struct i2c_mm *mm, const struct device *i2c, uint16_t addr, i2c_mm_serve_cb serve)
{
	int ret;

	if (!device_is_ready(i2c)) {
		LOG_ERR("%s not ready", i2c->name);
		return -ENODEV;
	}
	mm->i2c = i2c;
	mm->serve = serve;
	k_mutex_init(&mm->lock);
	mm->target.address = addr;
	mm->target.callbacks = &target_cb;
	ret = i2c_target_register(i2c, &mm->target);
	if (ret) {
		LOG_ERR("%s: cannot serve at 0x%02x: %d", i2c->name, addr, ret);
	}
	return ret;
}

static void transfer_self(struct i2c_mm *mm, const uint8_t *req, size_t req_len, uint8_t *rsp,
			  size_t rsp_len)
{
	uint8_t out[I2C_MM_MSG_MAX];
	size_t len = mm->serve(req, req_len, out);

	memset(&out[len], 0xff, sizeof(out) - len);
	memcpy(rsp, out, rsp_len);
}

int i2c_mm_transfer(struct i2c_mm *mm, uint16_t addr, const void *req, size_t req_len, void *rsp,
		    size_t rsp_len)
{
	k_timepoint_t end = sys_timepoint_calc(K_MSEC(CONFIG_I2C_MM_RETRY_MS));
	k_timepoint_t nack_end = sys_timepoint_calc(K_MSEC(CONFIG_I2C_MM_NACK_RETRY_MS));

	if (req_len > I2C_MM_MSG_MAX || rsp_len > I2C_MM_MSG_MAX) {
		return -EINVAL;
	}
	if (addr == mm->target.address) {
		transfer_self(mm, req, req_len, rsp, rsp_len);
		return 0;
	}

	for (uint32_t i = 1;; i++) {
		bool nack;
		int ret;

		/* re-registering must not hit another thread's transfer */
		k_mutex_lock(&mm->lock, K_FOREVER);
		ret = i2c_write_read(mm->i2c, addr, req, req_len, rsp, rsp_len);
		if (IS_ENABLED(CONFIG_I2C_MM_TARGET_REREGISTER)) {
			(void)target_reregister(mm->i2c, &mm->target);
		}
		nack = ret == -ENXIO;
		if (ret == 0) {
			mm->stats.sent++;
		} else if (ret == -EAGAIN || ret == -EBUSY || nack) {
			mm->stats.retries++;
		}
		k_mutex_unlock(&mm->lock);

		if (nack) {
			if (sys_timepoint_expired(nack_end)) {
				return -EIO;
			}
			k_sleep(K_MSEC(1));
			continue;
		}
		if ((ret != -EAGAIN && ret != -EBUSY) || sys_timepoint_expired(end)) {
			return ret;
		}
		LOG_WRN("0x%02x: request to 0x%02x attempt %u: %s, backing off", mm->target.address,
			addr, i, ret == -EAGAIN ? "lost arbitration" : "bus busy");
		/* out of the winner's START window, then wait for its STOP */
		k_sleep(K_MSEC(2 * MIN(i, 4)));
	}
}
