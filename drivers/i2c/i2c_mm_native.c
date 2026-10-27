/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Multi-master I2C port for native_sim (dts/bindings/i2c/twixt,native-i2c-mm.yaml).
 *
 * The wires are shared memory (i2c_mm_native_bottom.c). Target events raise this port's
 * interrupt line; a watcher thread plays the target's bus hardware.
 *
 * Not modelled: 10-bit addresses, target buffer mode, more than one port per
 * process, identical arbitration keys (decided by arrival order).
 */

#define DT_DRV_COMPAT twixt_native_i2c_mm

#include <string.h>
#include <irq_ctrl.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/i2c/i2c_mm_native.h>
#include "i2c_mm_native_bottom.h"

LOG_MODULE_REGISTER(i2c_mm_native, CONFIG_I2C_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1, "one multi-master port per process");

#define MM_ISR_TIMEOUT_MS 50
#define MM_POLL_US        50
#define MM_IDLE_POLL_US   1000 /* a START lasts longer than this */

enum mm_event {
	MM_EV_ADDR_W,
	MM_EV_ADDR_R, /* delivers the first byte */
	MM_EV_WRITE,
	MM_EV_READ,
	MM_EV_STOP,
};

struct mm_config {
	uint8_t bus_id;
	uint8_t irq_line;
	uint32_t clock_frequency;
	uint32_t bus_timeout_us;
	uint32_t start_window_us;
	bool loopback;
	void (*irq_config)(const struct device *dev);
};

struct mm_data {
	struct k_mutex xfer_lock;
	struct k_sem isr_done;
	uint32_t config;
	uint32_t bit_us;
	uint32_t join_us;
	struct i2c_target_config *target;
	struct i2c_mm_native_stats stats;

	/* filled in by the watcher before it raises the interrupt */
	enum mm_event ev;
	uint8_t byte;
	int result;

	struct k_thread watcher;
	K_KERNEL_STACK_MEMBER(watcher_stack, 1024);
};

static int64_t mm_now_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

static void mm_isr(const void *arg)
{
	const struct device *dev = arg;
	struct mm_data *d = dev->data;
	struct i2c_target_config *tc = d->target;
	const struct i2c_target_callbacks *cb = tc ? tc->callbacks : NULL;
	int ret = -ENODEV;

	d->stats.irqs++;

	if (cb != NULL) {
		switch (d->ev) {
		case MM_EV_ADDR_W:
			ret = cb->write_requested ? cb->write_requested(tc) : 0;
			break;
		case MM_EV_ADDR_R:
			ret = cb->read_requested ? cb->read_requested(tc, &d->byte) : -ENOTSUP;
			break;
		case MM_EV_WRITE:
			ret = cb->write_received ? cb->write_received(tc, d->byte) : -ENOTSUP;
			break;
		case MM_EV_READ:
			ret = cb->read_processed ? cb->read_processed(tc, &d->byte) : -ENOTSUP;
			break;
		case MM_EV_STOP:
			ret = cb->stop ? cb->stop(tc) : 0;
			break;
		}
	}

	d->result = ret;
	k_sem_give(&d->isr_done);
}

static void mm_watcher(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	const struct mm_config *cfg = dev->config;
	struct mm_data *d = dev->data;
	int ev;
	uint8_t byte;

	while (1) {
		if (!i2c_mm_native_bottom_fetch(&ev, &byte)) {
			k_usleep(i2c_mm_native_bottom_active() ? MM_POLL_US : MM_IDLE_POLL_US);
			continue;
		}

		d->ev = ev;
		d->byte = byte;
		k_sem_reset(&d->isr_done);
		hw_irq_ctrl_raise_im_from_sw(cfg->irq_line);

		if (k_sem_take(&d->isr_done, K_MSEC(MM_ISR_TIMEOUT_MS)) != 0) {
			LOG_ERR("target interrupt not serviced");
			d->result = -ETIMEDOUT;
		}
		i2c_mm_native_bottom_answer(d->result, d->byte);
	}
}

/* Waits for the target's ISR, as SCL stretching */
static int mm_port_event(int tslot, enum mm_event ev, uint8_t *byte)
{
	uint8_t answer;
	int result;

	i2c_mm_native_bottom_post(tslot, ev, byte ? *byte : 0);

	if (!WAIT_FOR(i2c_mm_native_bottom_answered(tslot, &result, &answer),
		      MM_ISR_TIMEOUT_MS * USEC_PER_MSEC, k_usleep(MM_POLL_US))) {
		i2c_mm_native_bottom_cancel(tslot);
		LOG_ERR("target in slot %d does not answer", tslot);
		return -ETIMEDOUT;
	}
	if (byte) {
		*byte = answer;
	}
	return result;
}

static int mm_bus_acquire(const struct device *dev, const uint8_t *key, int key_len)
{
	const struct mm_config *cfg = dev->config;
	struct mm_data *d = dev->data;
	int64_t deadline = mm_now_us() + cfg->bus_timeout_us;

	for (;;) {
		int r = i2c_mm_native_bottom_start(key, key_len, d->join_us);

		if (r == I2C_MM_NATIVE_BOTTOM_OWNER) {
			break;
		}
		if (r == I2C_MM_NATIVE_BOTTOM_STUCK) {
			i2c_mm_native_bottom_set_stuck(0);
			d->stats.recoveries++;
			LOG_WRN("bus stuck, recovered it");
			continue;
		}
		if (r == I2C_MM_NATIVE_BOTTOM_LOST) {
			d->stats.arb_lost++;
			return -EAGAIN;
		}
		if (mm_now_us() >= deadline) {
			d->stats.bus_timeouts++;
			return -EBUSY;
		}
		k_usleep(MAX(d->bit_us, MM_POLL_US));
	}

	for (;;) {
		int r = i2c_mm_native_bottom_confirm(d->join_us);

		if (r > 0) {
			return 0;
		}
		if (r == 0) {
			d->stats.arb_lost++;
			return -EAGAIN;
		}
		k_usleep(MM_POLL_US);
	}
}

static int mm_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
		       uint16_t addr)
{
	const struct mm_config *cfg = dev->config;
	struct mm_data *d = dev->data;
	const uint32_t byte_us = 9U * d->bit_us;
	uint8_t key[I2C_MM_NATIVE_BOTTOM_KEY_MAX];
	int key_len = 1;
	bool addressed = false;
	int tslot;
	int ret = 0;

	if (num_msgs == 0) {
		return 0;
	}
	for (int i = 0; i < num_msgs; i++) {
		if (msgs[i].flags & I2C_MSG_ADDR_10_BITS) {
			return -ENOTSUP;
		}
	}

	/* arbitration key: what is on the wire while masters compete */
	key[0] = (addr << 1) | (msgs[0].flags & I2C_MSG_READ);
	if (!(msgs[0].flags & I2C_MSG_READ)) {
		uint8_t n = MIN(msgs[0].len, I2C_MM_NATIVE_BOTTOM_KEY_MAX - 1);

		memcpy(&key[1], msgs[0].buf, n);
		key_len += n;
	}

	k_mutex_lock(&d->xfer_lock, K_FOREVER);

	ret = mm_bus_acquire(dev, key, key_len);
	if (ret) {
		goto out;
	}

	tslot = i2c_mm_native_bottom_find_target(addr, cfg->loopback);

	for (int i = 0; i < num_msgs && ret == 0; i++) {
		bool rd = msgs[i].flags & I2C_MSG_READ;
		uint8_t b = 0;

		k_usleep((i == 0 ? 0 : d->bit_us) + byte_us); /* (repeated) START + address */

		if (tslot < 0 || mm_port_event(tslot, rd ? MM_EV_ADDR_R : MM_EV_ADDR_W, &b)) {
			d->stats.nacks++;
			ret = -EIO;
			break;
		}
		addressed = true;

		for (uint32_t j = 0; j < msgs[i].len && ret == 0; j++) {
			if (rd) {
				if (j > 0 && mm_port_event(tslot, MM_EV_READ, &b)) {
					ret = -EIO;
				}
				msgs[i].buf[j] = b;
			} else {
				b = msgs[i].buf[j];
				if (mm_port_event(tslot, MM_EV_WRITE, &b)) {
					d->stats.nacks++;
					ret = -EIO;
				}
			}
			k_usleep(byte_us);
		}
	}

	if (addressed) {
		mm_port_event(tslot, MM_EV_STOP, NULL);
	}
	k_usleep(d->bit_us); /* STOP */
	i2c_mm_native_bottom_release();
out:
	k_mutex_unlock(&d->xfer_lock);
	return ret;
}

static int mm_set_speed(const struct device *dev, uint32_t speed)
{
	const struct mm_config *cfg = dev->config;
	struct mm_data *d = dev->data;

	switch (speed) {
	case I2C_SPEED_STANDARD:
		d->bit_us = 10;
		break;
	case I2C_SPEED_FAST:
		d->bit_us = 3; /* 2.5 us rounded up */
		break;
	case I2C_SPEED_FAST_PLUS:
		d->bit_us = 1;
		break;
	default:
		return -ENOTSUP;
	}
	d->join_us = MAX(MAX(d->bit_us, cfg->start_window_us), k_ticks_to_us_ceil32(1));
	return 0;
}

static int mm_configure(const struct device *dev, uint32_t dev_config)
{
	struct mm_data *d = dev->data;
	int ret;

	if (!(dev_config & I2C_MODE_CONTROLLER)) {
		return -ENOTSUP;
	}
	ret = mm_set_speed(dev, I2C_SPEED_GET(dev_config));
	if (ret == 0) {
		d->config = dev_config;
	}
	return ret;
}

static int mm_get_config(const struct device *dev, uint32_t *dev_config)
{
	struct mm_data *d = dev->data;

	*dev_config = d->config;
	return 0;
}

static int mm_target_register(const struct device *dev, struct i2c_target_config *tc)
{
	struct mm_data *d = dev->data;
	int ret;

	if (tc == NULL || tc->callbacks == NULL) {
		return -EINVAL;
	}
	if (tc->flags & I2C_TARGET_FLAGS_ADDR_10_BITS) {
		return -ENOTSUP;
	}
	if (d->target != NULL) {
		return -EBUSY;
	}

	ret = i2c_mm_native_bottom_set_target(tc->address);
	if (ret == 0) {
		d->target = tc;
	}
	return ret;
}

static int mm_target_unregister(const struct device *dev, struct i2c_target_config *tc)
{
	struct mm_data *d = dev->data;

	if (d->target != tc) {
		return -EINVAL;
	}
	i2c_mm_native_bottom_set_target(-1);
	d->target = NULL;
	return 0;
}

static int mm_recover_bus(const struct device *dev)
{
	struct mm_data *d = dev->data;

	i2c_mm_native_bottom_set_stuck(0);
	d->stats.recoveries++;
	return 0;
}

void i2c_mm_native_stick_bus(const struct device *dev)
{
	ARG_UNUSED(dev);

	i2c_mm_native_bottom_set_stuck(1);
}

void i2c_mm_native_get_stats(const struct device *dev, struct i2c_mm_native_stats *stats)
{
	const struct mm_data *d = dev->data;

	*stats = d->stats;
}

int i2c_mm_native_wait_targets(const struct device *dev, int count)
{
	ARG_UNUSED(dev);

	return WAIT_FOR(i2c_mm_native_bottom_targets() >= count, 10 * USEC_PER_SEC, k_msleep(1))
		       ? 0
		       : -ETIMEDOUT;
}

int i2c_mm_native_wait_done(const struct device *dev, int count)
{
	ARG_UNUSED(dev);

	i2c_mm_native_bottom_set_done();
	return WAIT_FOR(i2c_mm_native_bottom_done_count() >= count, 10 * USEC_PER_SEC, k_msleep(1))
		       ? 0
		       : -ETIMEDOUT;
}

void i2c_mm_native_wait_round(const struct device *dev, uint32_t round, uint32_t period_ms)
{
	int64_t due_us = i2c_mm_native_bottom_epoch_us() + 1000000LL + round * period_ms * 1000LL;
	int64_t wait_us;

	/*
	 * Not one long sleep: a sleep is simulated time, and a process that is behind the
	 * wall clock catches up by running it faster, so the sleep would end too early.
	 */
	while ((wait_us = due_us - i2c_mm_native_bottom_wall_us()) > 0) {
		k_usleep(wait_us > 400 ? wait_us / 2 : 200);
	}
}

static int mm_init(const struct device *dev)
{
	const struct mm_config *cfg = dev->config;
	struct mm_data *d = dev->data;
	uint32_t speed;
	int ret = i2c_mm_native_bottom_attach(cfg->bus_id);

	if (ret < 0) {
		LOG_ERR("cannot join bus %u: %d", cfg->bus_id, ret);
		return -EIO;
	}

	k_mutex_init(&d->xfer_lock);
	k_sem_init(&d->isr_done, 0, 1);

	if (cfg->clock_frequency >= 1000000) {
		speed = I2C_SPEED_FAST_PLUS;
	} else if (cfg->clock_frequency >= 400000) {
		speed = I2C_SPEED_FAST;
	} else {
		speed = I2C_SPEED_STANDARD;
	}
	mm_configure(dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(speed));

	cfg->irq_config(dev);

	k_thread_create(&d->watcher, d->watcher_stack, K_KERNEL_STACK_SIZEOF(d->watcher_stack),
			mm_watcher, (void *)dev, NULL, NULL, K_HIGHEST_APPLICATION_THREAD_PRIO, 0,
			K_NO_WAIT);
	k_thread_name_set(&d->watcher, "i2c_mm_watcher");
	return 0;
}

static DEVICE_API(i2c, mm_api) = {
	.configure = mm_configure,
	.get_config = mm_get_config,
	.transfer = mm_transfer,
	.target_register = mm_target_register,
	.target_unregister = mm_target_unregister,
	.recover_bus = mm_recover_bus,
};

#define MM_INST(n)                                                                                 \
	static void mm_irq_config_##n(const struct device *dev)                                    \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_PROP(n, irq_line), DT_INST_PROP(n, irq_priority), mm_isr,      \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_PROP(n, irq_line));                                             \
	}                                                                                          \
	static struct mm_data mm_data_##n;                                                         \
	static const struct mm_config mm_config_##n = {                                            \
		.bus_id = DT_INST_PROP(n, bus_id),                                                 \
		.irq_line = DT_INST_PROP(n, irq_line),                                             \
		.clock_frequency = DT_INST_PROP_OR(n, clock_frequency, 100000),                    \
		.bus_timeout_us = DT_INST_PROP(n, bus_timeout_us),                                 \
		.start_window_us = DT_INST_PROP(n, start_window_us),                               \
		.loopback = DT_INST_PROP(n, loopback),                                             \
		.irq_config = mm_irq_config_##n,                                                   \
	};                                                                                         \
	I2C_DEVICE_DT_INST_DEFINE(n, mm_init, NULL, &mm_data_##n, &mm_config_##n, POST_KERNEL,     \
				  CONFIG_I2C_INIT_PRIORITY, &mm_api);

DT_INST_FOREACH_STATUS_OKAY(MM_INST)
