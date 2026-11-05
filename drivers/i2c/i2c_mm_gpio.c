/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Multi-master I2C from a target-only I2C block and two GPIOs on its pins: how it works from the
 * outside is in dts/bindings/i2c/twixt,i2c-mm-gpio.yaml.
 *
 * The high phase of a bit runs with interrupts locked, so the bit is sampled
 * while SCL is high and no other controller's clock cycle is missed.
 *
 * Every phase ends at a time on a counter, counted from the edge that began it, so the time the
 * GPIO calls take is part of the phase; a bit ends a period after SCL fell, however slowly SCL
 * rose, but stays high for the minimum high time at least. Counted loops would change their
 * speed with the code around them (e.g. through an XIP cache). The waits poll SCL, for clock
 * stretching and other controllers.
 *
 * The pins and the counter are in i2c_mm_gpio_port.h, in the folder CONFIG_I2C_MM_GPIO_PORT names.
 */

#define DT_DRV_COMPAT twixt_i2c_mm_gpio

#include <zephyr/device.h>
#include <zephyr/dt-bindings/i2c/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(i2c_mm_gpio, CONFIG_I2C_LOG_LEVEL);

#include "i2c_mm_gpio_port.h"
#include "target_restore.h"

#ifndef MM_GPIO_PORT_CHECK
#define MM_GPIO_PORT_CHECK(n)
#endif

#define STRETCH_LOCKED_US 100
#define RECOVERY_CLOCKS   9
#define LOW_SHARE         55 /* % of the period, like the minimum 4.7 us low to 4.0 us high */
#define START_STOP_US     5  /* > tSU;STA 4.7 us, tHD;STA and tSU;STO 4.0 us */

struct mm_gpio_config {
	const struct device *target;
	const struct pinctrl_dev_config *pcfg; /* NULL: the target takes its pins back itself */
	int target_irq;
	struct gpio_dt_spec scl;
	struct gpio_dt_spec sda;
	uint32_t bitrate;
	uint32_t bus_free_us;
	uint32_t bus_timeout_us;
	uint32_t stretch_timeout_us;
};

struct mm_gpio_data {
	struct k_mutex lock;
	struct i2c_target_config *target_cfg; /* registered at the target block */
	struct i2c_target_config *user_cfg;   /* registered here */
#ifdef CONFIG_I2C_TARGET_BUFFER_MODE
	struct i2c_target_config own_cfg;
	const struct mm_gpio_config *cfg;
#endif
	uint32_t dev_config;
	uint32_t cycles_per_us;
	uint32_t t_low; /* in cycles of the counter, like all times below */
	uint32_t t_high;
	uint32_t t_high_min;
	uint32_t scl_fell; /* when this controller last pulled SCL low */
};

static inline void scl_set(const struct mm_gpio_config *cfg, int v)
{
	pin_set(&cfg->scl, v);
}

static inline void sda_set(const struct mm_gpio_config *cfg, int v)
{
	pin_set(&cfg->sda, v);
}

static inline int scl_get(const struct mm_gpio_config *cfg)
{
	return pin_get(&cfg->scl);
}

static inline int sda_get(const struct mm_gpio_config *cfg)
{
	return pin_get(&cfg->sda);
}

/* A minimum time: one tick more, since the edge may come late in the tick it was read in */
static inline uint32_t us(const struct mm_gpio_data *d, uint32_t t)
{
	return t * d->cycles_per_us + 1;
}

static inline bool before(uint32_t deadline)
{
	return (int32_t)(now() - deadline) < 0;
}

static void hold_until(uint32_t deadline)
{
	while (before(deadline)) {
	}
}

static void scl_fall(struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	scl_set(cfg, 0);
	d->scl_fell = now();
}

static bool wait_scl_high(const struct mm_gpio_config *cfg, uint32_t deadline)
{
	while (!scl_get(cfg)) {
		if (!before(deadline)) {
			return false;
		}
	}
	return true;
}

/* Releases SCL, waits out stretching and returns with interrupts locked and SCL high. SCL is
 * high only with interrupts locked, else a preempted frame looks like a free bus. While a target
 * stretches, interrupts run: the target holds SCL low. This master must not pull it low itself: a
 * target letting go just before that would see a clock this master does not count, and every bit
 * after it would be one off.
 */
static int scl_rise(struct mm_gpio_data *d, const struct mm_gpio_config *cfg, unsigned int *key)
{
	uint32_t tries = DIV_ROUND_UP(cfg->stretch_timeout_us, STRETCH_LOCKED_US);

	*key = irq_lock();
	scl_set(cfg, 1);
	while (!wait_scl_high(cfg, now() + us(d, STRETCH_LOCKED_US))) {
		if (tries-- == 0U) {
			irq_unlock(*key);
			return -ETIMEDOUT;
		}
		irq_unlock(*key);
		*key = irq_lock();
	}
	return 0;
}

/* One bit; *bit is what was on SDA. With arb, a 1 that reads 0 releases both lines. */
static int clock_bit(struct mm_gpio_data *d, const struct mm_gpio_config *cfg, int out, bool arb,
		     int *bit)
{
	uint32_t high_end;
	unsigned int key;
	int ret;

	sda_set(cfg, out);
	hold_until(d->scl_fell + d->t_low);
	ret = scl_rise(d, cfg, &key);
	if (ret) {
		sda_set(cfg, 1);
		return ret;
	}
	high_end = d->scl_fell + d->t_low + d->t_high;
	if ((int32_t)(now() + d->t_high_min - high_end) > 0) {
		high_end = now() + d->t_high_min;
	}
	*bit = sda_get(cfg);
	if (arb && out && !*bit) {
		irq_unlock(key);
		return -EAGAIN;
	}
	while (before(high_end) && scl_get(cfg)) {
	}
	scl_fall(d, cfg);
	irq_unlock(key);
	return 0;
}

/* 0: ACK, 1: NACK */
static int write_byte(struct mm_gpio_data *d, const struct mm_gpio_config *cfg, uint8_t val)
{
	int bit;
	int ret;

	for (int i = 7; i >= 0; i--) {
		ret = clock_bit(d, cfg, (val >> i) & 1, true, &bit);
		if (ret) {
			return ret;
		}
	}
	ret = clock_bit(d, cfg, 1, false, &bit);
	return ret ? ret : bit;
}

static int read_byte(struct mm_gpio_data *d, const struct mm_gpio_config *cfg, uint8_t *val,
		     bool ack)
{
	int bit;
	int ret;

	*val = 0;
	for (int i = 0; i < 8; i++) {
		ret = clock_bit(d, cfg, 1, false, &bit);
		if (ret) {
			return ret;
		}
		*val = (*val << 1) | bit;
	}
	/* Another reader may ACK when this controller sends its final NACK. */
	return clock_bit(d, cfg, ack ? 0 : 1, true, &bit);
}

/* -EBUSY: someone else's transfer; -EIO: SDA held low with SCL high all along. A frame can pass
 * unseen while this is preempted, and unregistering the target in it breaks it: the idle time
 * begins anew after a gap between two reads.
 */
static int bus_free(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	uint32_t end = now() + us(d, cfg->bus_timeout_us);
	uint32_t idle_since = now();
	uint32_t last = idle_since;
	bool stuck = true;

	while (before(end)) {
		int scl = scl_get(cfg);
		int sda = sda_get(cfg);
		uint32_t t = now();

		if (!(scl && sda) || t - last > d->t_low) {
			idle_since = t;
		} else if (t - idle_since >= us(d, cfg->bus_free_us)) {
			return 0;
		}
		last = t;
		stuck = stuck && scl && !sda;
	}
	return stuck ? -EIO : -EBUSY;
}

/* SCL high, SDA high -> SDA low -> SCL low. The bus is free only if it stays so up to the START,
 * after whatever ran since bus_free(): the last bus_free_us run with interrupts locked.
 */
static int start(struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	unsigned int key = irq_lock();
	uint32_t end = now() + us(d, cfg->bus_free_us);

	while (before(end)) {
		if (!scl_get(cfg) || !sda_get(cfg)) {
			irq_unlock(key);
			return -EBUSY;
		}
	}
	sda_set(cfg, 0);
	hold_until(now() + us(d, START_STOP_US));
	scl_fall(d, cfg);
	irq_unlock(key);
	return 0;
}

static int restart(struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	unsigned int key;
	int ret;

	sda_set(cfg, 1);
	hold_until(d->scl_fell + d->t_low);
	ret = scl_rise(d, cfg, &key);
	if (ret) {
		return ret;
	}
	hold_until(now() + us(d, START_STOP_US));
	sda_set(cfg, 0);
	hold_until(now() + us(d, START_STOP_US));
	scl_fall(d, cfg);
	irq_unlock(key);
	return 0;
}

/* A STOP up to its last edge: SCL high, SDA low */
static void stop_begin(struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	unsigned int key;

	sda_set(cfg, 0);
	hold_until(d->scl_fell + d->t_low);
	if (scl_rise(d, cfg, &key) == 0) {
		irq_unlock(key);
	}
	hold_until(now() + us(d, START_STOP_US));
}

static void stop(struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	stop_begin(d, cfg);
	sda_set(cfg, 1);
	hold_until(now() + d->t_low);
}

static void release(const struct mm_gpio_config *cfg)
{
	sda_set(cfg, 1);
	scl_set(cfg, 1);
}

static int recover(struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	unsigned int key;

	sda_set(cfg, 1);
	for (int i = 0; i < RECOVERY_CLOCKS && !sda_get(cfg); i++) {
		scl_fall(d, cfg);
		hold_until(d->scl_fell + d->t_low);
		if (scl_rise(d, cfg, &key)) {
			return -EIO;
		}
		irq_unlock(key);
		hold_until(now() + d->t_high);
	}
	scl_fall(d, cfg);
	stop(d, cfg);
	return sda_get(cfg) ? 0 : -EIO;
}

#define PIN_FLAGS (GPIO_INPUT | GPIO_OUTPUT | GPIO_OUTPUT_INIT_HIGH | GPIO_OPEN_DRAIN)

/*
 * The target block's interrupt is off while its target is not registered. An unregistered block
 * still sees the bus: the DesignWare one (Raspberry Pi Pico) goes on signalling another master's
 * START, which its driver clears only in target mode, and the interrupt never ends.
 */
static inline void target_irq_set(const struct mm_gpio_config *cfg, bool on)
{
#ifdef MM_GPIO_TARGET_HANDOFF
	if (target_handoff_matches(cfg->target)) {
		return; /* The backend owns its dynamically allocated interrupts. */
	}
#endif
	if (cfg->target_irq < 0) {
		return;
	}
	if (on) {
		irq_enable(cfg->target_irq);
	} else {
		irq_disable(cfg->target_irq);
	}
}

static inline bool handoff(const struct mm_gpio_config *cfg)
{
#ifdef MM_GPIO_TARGET_HANDOFF
	return target_handoff_matches(cfg->target);
#else
	return false;
#endif
}

static void pins_give(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg);

/* stuck: bus_free() saw SDA held low throughout, so no peer's frame can still be running.
 * A failed handoff takes nothing and must not be followed by pins_give().
 */
static int pins_take(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg, bool stuck)
{
	int ret = 0;

#ifdef MM_GPIO_TARGET_HANDOFF
	if (handoff(cfg)) {
		return target_handoff_take(cfg->target, cfg->bus_free_us, stuck);
	}
#endif
	if (d->target_cfg) {
		target_irq_set(cfg, false);
		ret = i2c_target_unregister(cfg->target, d->target_cfg);
		if (ret != 0) {
			target_irq_set(cfg, true);
			return ret;
		}
	}

	ret = gpio_pin_configure_dt(&cfg->scl, PIN_FLAGS);
	ret = ret ? ret : gpio_pin_configure_dt(&cfg->sda, PIN_FLAGS);
	if (ret != 0) {
		pins_give(d, cfg);
	}
	return ret;
}

/* The target takes the pins back before the GPIOs release them: after a STOP begun with
 * stop_begin(), releasing SDA ends it, and the target already sees the next START
 */
static int pins_to_target(const struct mm_gpio_config *cfg)
{
	return cfg->pcfg != NULL ? pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT) : 0;
}

static void pins_give(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
#ifdef MM_GPIO_TARGET_HANDOFF
	if (target_handoff_matches(cfg->target)) {
		release(cfg);
		target_handoff_give(cfg->target);
		return;
	}
#endif
	/* On the Pico pinctrl releases SDA, completing STOP. Enable the target first. */
	int ret = d->target_cfg ? target_reregister(cfg->target, d->target_cfg) : 0;

	if (d->target_cfg && ret == 0) {
		target_irq_set(cfg, true);
	}
	/* Do not pause between pinctrl releasing SDA and selecting its target function. */
	unsigned int key = irq_lock();
	int pin_ret = pins_to_target(cfg);

	release(cfg);
	irq_unlock(key);
	if (d->target_cfg) {
		target_restore_report(ret ? ret : pin_ret);
	}
}

static bool msg_addressed(const struct i2c_msg *msgs, uint8_t m)
{
	return m == 0 || (msgs[m - 1].flags & I2C_MSG_STOP) || (msgs[m].flags & I2C_MSG_RESTART) ||
	       ((msgs[m].flags ^ msgs[m - 1].flags) & I2C_MSG_RW_MASK);
}

static int msgs_run(struct mm_gpio_data *d, const struct mm_gpio_config *cfg, struct i2c_msg *msgs,
		    uint8_t num_msgs, uint16_t addr)
{
	int ret = start(d, cfg);

	for (uint8_t m = 0; m < num_msgs && ret == 0; m++) {
		bool rd = (msgs[m].flags & I2C_MSG_RW_MASK) == I2C_MSG_READ;
		bool addressed = msg_addressed(msgs, m);

		/* a STOP was followed by a fresh START already */
		if (m > 0 && addressed && !(msgs[m - 1].flags & I2C_MSG_STOP)) {
			ret = restart(d, cfg);
			if (ret) {
				break;
			}
		}
		if (addressed) {
			ret = write_byte(d, cfg, (addr << 1) | rd);
			if (ret == 1) {
				ret = -ENXIO;
				break;
			}
		}
		for (uint32_t i = 0; i < msgs[m].len && ret == 0; i++) {
			if (rd) {
				bool last = i + 1 == msgs[m].len &&
					    (m + 1 == num_msgs || msg_addressed(msgs, m + 1));

				ret = read_byte(d, cfg, &msgs[m].buf[i], !last);
			} else {
				ret = write_byte(d, cfg, msgs[m].buf[i]);
				if (ret == 1) {
					ret = -EIO;
				}
			}
		}
		if (ret == 0 && (msgs[m].flags & I2C_MSG_STOP) && m + 1 < num_msgs) {
			stop(d, cfg);
			ret = bus_free(d, cfg);
			if (ret == 0) {
				ret = start(d, cfg);
			}
		}
	}

	if (ret == 0 || ret == -ENXIO || ret == -EIO || ret == -ETIMEDOUT) {
		stop_begin(d, cfg); /* pins_give() releases SDA: the STOP */
	}
	return ret;
}

static int mm_gpio_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
			    uint16_t addr)
{
	const struct mm_gpio_config *cfg = dev->config;
	struct mm_gpio_data *d = dev->data;
	int ret;

	if (d->dev_config & I2C_ADDR_10_BITS) {
		return -ENOTSUP;
	}
	if (num_msgs == 0) {
		return 0;
	}

	k_mutex_lock(&d->lock, K_FOREVER);
	ret = bus_free(d, cfg);
	if (ret == 0 || ret == -EIO) {
		bool stuck = ret == -EIO;

		ret = pins_take(d, cfg, stuck);
		if (ret != 0) {
			k_mutex_unlock(&d->lock);
			return ret;
		}
		if (stuck) {
			LOG_WRN("SDA stuck low, recovering");
			ret = ret ? ret : recover(d, cfg);
			ret = ret ? ret : bus_free(d, cfg);
		}
		ret = ret ? ret : msgs_run(d, cfg, msgs, num_msgs, addr);
		pins_give(d, cfg);
	}
	k_mutex_unlock(&d->lock);
	return ret;
}

static int mm_gpio_configure(const struct device *dev, uint32_t dev_config)
{
	struct mm_gpio_data *d = dev->data;
	uint32_t period;
	uint32_t rate;
	uint32_t high_min_ns;

#ifdef MM_GPIO_TARGET_HANDOFF
	const struct mm_gpio_config *cfg = dev->config;

	if (target_handoff_matches(cfg->target) &&
	    I2C_SPEED_GET(dev_config) != I2C_SPEED_STANDARD) {
		return -ENOTSUP;
	}
#endif
	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		rate = I2C_BITRATE_STANDARD;
		high_min_ns = 4000;
		break;
	case I2C_SPEED_FAST:
		rate = I2C_BITRATE_FAST;
		high_min_ns = 600;
		break;
	default:
		return -ENOTSUP;
	}
	if (!(dev_config & I2C_MODE_CONTROLLER) || (dev_config & I2C_ADDR_10_BITS)) {
		return -ENOTSUP;
	}

	k_mutex_lock(&d->lock, K_FOREVER);
	period = DIV_ROUND_UP(USEC_PER_SEC * d->cycles_per_us, rate);
	d->t_low = period * LOW_SHARE / 100 + 1;
	d->t_high = period - d->t_low;
	d->t_high_min = DIV_ROUND_UP(high_min_ns * d->cycles_per_us, NSEC_PER_USEC) + 1;
	d->dev_config = dev_config;
	k_mutex_unlock(&d->lock);
	return 0;
}

static int mm_gpio_get_config(const struct device *dev, uint32_t *dev_config)
{
	struct mm_gpio_data *d = dev->data;

	*dev_config = d->dev_config;
	return 0;
}

#ifdef CONFIG_I2C_TARGET_BUFFER_MODE
/*
 * A target block in buffer mode may not report a STOP (nRF TWIS), and "write done" is the same
 * after a STOP and after a repeated START. The lines tell them apart. After a repeated START the
 * read's address is arriving: its last clock may still be high when this runs, but within a bit
 * SCL falls and the block holds it. After a STOP both lines stay high; the next master's START
 * comes a bus-free time later at least.
 */
static bool stays_idle(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	uint32_t end = now() + 2 * (d->t_low + d->t_high);

	while (before(end)) {
		if (!scl_get(cfg) || !sda_get(cfg)) {
			return false;
		}
	}
	return true;
}

static struct i2c_target_config *user_of(struct i2c_target_config *own)
{
	return CONTAINER_OF(own, struct mm_gpio_data, own_cfg)->user_cfg;
}

static void own_buf_write_received(struct i2c_target_config *own, uint8_t *buf, uint32_t len)
{
	struct mm_gpio_data *d = CONTAINER_OF(own, struct mm_gpio_data, own_cfg);
	struct i2c_target_config *tc = d->user_cfg;

	tc->callbacks->buf_write_received(tc, buf, len);
	if (stays_idle(d, d->cfg) && tc->callbacks->stop != NULL) {
		tc->callbacks->stop(tc);
	}
}

static int own_buf_read_requested(struct i2c_target_config *own, uint8_t **buf, uint32_t *len)
{
	struct i2c_target_config *tc = user_of(own);

	return tc->callbacks->buf_read_requested(tc, buf, len);
}

static int own_write_requested(struct i2c_target_config *own)
{
	struct i2c_target_config *tc = user_of(own);

	return tc->callbacks->write_requested(tc);
}

static int own_write_received(struct i2c_target_config *own, uint8_t val)
{
	struct i2c_target_config *tc = user_of(own);

	return tc->callbacks->write_received(tc, val);
}

static int own_read_requested(struct i2c_target_config *own, uint8_t *val)
{
	struct i2c_target_config *tc = user_of(own);

	return tc->callbacks->read_requested(tc, val);
}

static int own_read_processed(struct i2c_target_config *own, uint8_t *val)
{
	struct i2c_target_config *tc = user_of(own);

	return tc->callbacks->read_processed(tc, val);
}

static int own_stop(struct i2c_target_config *own)
{
	struct i2c_target_config *tc = user_of(own);

	return tc->callbacks->stop != NULL ? tc->callbacks->stop(tc) : 0;
}

static const struct i2c_target_callbacks own_callbacks = {
	.write_requested = own_write_requested,
	.write_received = own_write_received,
	.read_requested = own_read_requested,
	.read_processed = own_read_processed,
	.buf_write_received = own_buf_write_received,
	.buf_read_requested = own_buf_read_requested,
	.stop = own_stop,
};

static struct i2c_target_config *block_cfg(struct mm_gpio_data *d, const struct mm_gpio_config *cfg,
					   const struct i2c_target_config *tc)
{
	d->cfg = cfg;
	d->own_cfg = *tc;
	d->own_cfg.callbacks = &own_callbacks;
	return &d->own_cfg;
}
#else
static struct i2c_target_config *block_cfg(struct mm_gpio_data *d, const struct mm_gpio_config *cfg,
					   struct i2c_target_config *tc)
{
	return tc;
}
#endif

static int mm_gpio_target_register(const struct device *dev, struct i2c_target_config *tc)
{
	const struct mm_gpio_config *cfg = dev->config;
	struct mm_gpio_data *d = dev->data;
	int ret = -EBUSY;

	k_mutex_lock(&d->lock, K_FOREVER);
	if (d->user_cfg == NULL || d->user_cfg == tc) {
		struct i2c_target_config *block = block_cfg(d, cfg, tc);

		d->user_cfg = tc;
		ret = i2c_target_register(cfg->target, block);
		if (ret == 0) {
			d->target_cfg = block;
			target_irq_set(cfg, true);
		} else if (d->target_cfg == NULL) {
			d->user_cfg = NULL;
		}
	}
	k_mutex_unlock(&d->lock);
	return ret;
}

static int mm_gpio_target_unregister(const struct device *dev, struct i2c_target_config *tc)
{
	const struct mm_gpio_config *cfg = dev->config;
	struct mm_gpio_data *d = dev->data;
	int ret;

	k_mutex_lock(&d->lock, K_FOREVER);
	if (d->user_cfg != tc) {
		ret = -EINVAL;
	} else {
		target_irq_set(cfg, false);
		ret = i2c_target_unregister(cfg->target, d->target_cfg);
	}
	if (ret == 0) {
		d->target_cfg = NULL;
		d->user_cfg = NULL;
	} else if (d->user_cfg == tc) {
		target_irq_set(cfg, true);
	}
	k_mutex_unlock(&d->lock);
	return ret;
}

static int mm_gpio_recover_bus(const struct device *dev)
{
	const struct mm_gpio_config *cfg = dev->config;
	struct mm_gpio_data *d = dev->data;
	int ret;

	k_mutex_lock(&d->lock, K_FOREVER);
	if (handoff(cfg)) {
		/* Also waits out a peer's frame to the target: taking the pins would cut it */
		ret = bus_free(d, cfg);
		ret = ret == 0 || ret == -EIO ? pins_take(d, cfg, ret == -EIO) : ret;
		if (ret) {
			k_mutex_unlock(&d->lock);
			return ret;
		}
	} else {
		ret = pins_take(d, cfg, false);
		if (ret != 0) {
			k_mutex_unlock(&d->lock);
			return ret;
		}
	}
	ret = ret ? ret : recover(d, cfg);
	pins_give(d, cfg);
	k_mutex_unlock(&d->lock);
	return ret;
}

static DEVICE_API(i2c, mm_gpio_api) = {
	.configure = mm_gpio_configure,
	.get_config = mm_gpio_get_config,
	.transfer = mm_gpio_transfer,
	.target_register = mm_gpio_target_register,
	.target_unregister = mm_gpio_target_unregister,
	.recover_bus = mm_gpio_recover_bus,
};

static int mm_gpio_init(const struct device *dev)
{
	const struct mm_gpio_config *cfg = dev->config;
	struct mm_gpio_data *d = dev->data;

	if (!device_is_ready(cfg->target) || !gpio_is_ready_dt(&cfg->scl) ||
	    !gpio_is_ready_dt(&cfg->sda)) {
		return -ENODEV;
	}
	k_mutex_init(&d->lock);
	d->cycles_per_us = counter_init();
	(void)pins_to_target(cfg);
	return mm_gpio_configure(dev, I2C_MODE_CONTROLLER |
					      I2C_SPEED_SET(cfg->bitrate > I2C_BITRATE_STANDARD
								    ? I2C_SPEED_FAST
								    : I2C_SPEED_STANDARD));
}

/* clang-format off */
#define MM_GPIO_INST(n)                                                                            \
	IF_ENABLED(DT_INST_PINCTRL_HAS_NAME(n, default), (PINCTRL_DT_INST_DEFINE(n);))             \
	MM_GPIO_PORT_CHECK(n)                                                                      \
	static struct mm_gpio_data mm_gpio_data_##n;                                               \
	static const struct mm_gpio_config mm_gpio_config_##n = {                                  \
		.target = DEVICE_DT_GET(DT_INST_PHANDLE(n, target)),                               \
		.target_irq = COND_CODE_1(DT_IRQ_HAS_IDX(DT_INST_PHANDLE(n, target), 0),           \
					  (DT_IRQN(DT_INST_PHANDLE(n, target))), (-1)),            \
		.pcfg = COND_CODE_1(DT_INST_PINCTRL_HAS_NAME(n, default),                          \
				    (PINCTRL_DT_INST_DEV_CONFIG_GET(n)), (NULL)),                  \
		.scl = GPIO_DT_SPEC_INST_GET(n, scl_gpios),                                        \
		.sda = GPIO_DT_SPEC_INST_GET(n, sda_gpios),                                        \
		.bitrate = DT_INST_PROP(n, clock_frequency),                                       \
		.bus_free_us = DT_INST_PROP(n, bus_free_us),                                       \
		.bus_timeout_us = DT_INST_PROP(n, bus_timeout_us),                                 \
		.stretch_timeout_us = DT_INST_PROP(n, stretch_timeout_us),                         \
	};                                                                                         \
	I2C_DEVICE_DT_INST_DEFINE(n, mm_gpio_init, NULL, &mm_gpio_data_##n, &mm_gpio_config_##n,   \
				  POST_KERNEL, CONFIG_I2C_MM_GPIO_INIT_PRIORITY, &mm_gpio_api);
/* clang-format on */

DT_INST_FOREACH_STATUS_OKAY(MM_GPIO_INST)
