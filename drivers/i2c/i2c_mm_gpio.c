/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Multi-master I2C from a target-only I2C block and two GPIOs on its pins
 * (dts/bindings/i2c/twixt,i2c-mm-gpio.yaml).
 *
 * The target block serves requests. For its own transfer the port unregisters
 * the target, which hands the pins back to the GPIOs, clocks the transfer in
 * software and registers the target again. Every bit is read back: a 1 that
 * reads 0 is lost arbitration (-EAGAIN). Meanwhile the target NACKs its address.
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
 * The counter is that of the timing functions. On the RP2040 it is the 1 MHz timer and the pins
 * are SIO registers: k_cycle_get_32() and the GPIO API take about 1 us per call there.
 */

#define DT_DRV_COMPAT twixt_i2c_mm_gpio

#include <zephyr/device.h>
#include <zephyr/dt-bindings/i2c/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/timing/timing.h>

#ifdef CONFIG_SOC_SERIES_RP2040
#include <hardware/structs/sio.h>
#include <hardware/structs/timer.h>
#endif

LOG_MODULE_REGISTER(i2c_mm_gpio, CONFIG_I2C_LOG_LEVEL);

#include "target_reregister.h"

#define STRETCH_LOCKED_US 100
#define RECOVERY_CLOCKS   9
#define LOW_SHARE         55 /* % of the period, like the minimum 4.7 us low to 4.0 us high */
#define START_STOP_US     5  /* > tSU;STA 4.7 us, tHD;STA and tSU;STO 4.0 us */

struct mm_gpio_config {
	const struct device *target;
	const struct pinctrl_dev_config *pcfg; /* NULL: the target takes its pins back itself */
	struct gpio_dt_spec scl;
	struct gpio_dt_spec sda;
	uint32_t bitrate;
	uint32_t bus_free_us;
	uint32_t bus_timeout_us;
	uint32_t stretch_timeout_us;
};

struct mm_gpio_data {
	struct k_mutex lock;
	struct i2c_target_config *target_cfg;
	uint32_t dev_config;
	uint32_t cycles_per_us;
	uint32_t t_low; /* in cycles of the counter, like all times below */
	uint32_t t_high;
	uint32_t t_high_min;
	uint32_t scl_fell; /* when this controller last pulled SCL low */
};

#ifdef CONFIG_SOC_SERIES_RP2040
/* Open drain: the output is 0, the output enable pulls the line low */
static inline void pin_set(const struct gpio_dt_spec *g, int v)
{
	if (v) {
		sio_hw->gpio_oe_clr = BIT(g->pin);
	} else {
		sio_hw->gpio_oe_set = BIT(g->pin);
	}
}

static inline int pin_get(const struct gpio_dt_spec *g)
{
	return (sio_hw->gpio_in >> g->pin) & 1;
}

static inline uint32_t now(void)
{
	return timer_hw->timerawl;
}

static uint32_t counter_init(void)
{
	return 1;
}
#else
static inline void pin_set(const struct gpio_dt_spec *g, int v)
{
	gpio_pin_set_raw(g->port, g->pin, v);
}

static inline int pin_get(const struct gpio_dt_spec *g)
{
	return gpio_pin_get_raw(g->port, g->pin);
}

static inline uint32_t now(void)
{
	return (uint32_t)timing_counter_get();
}

static uint32_t counter_init(void)
{
	timing_init();
	timing_start();
	return MAX(timing_freq_get() / USEC_PER_SEC, 1);
}
#endif

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
 * high only with interrupts locked, else a preempted frame looks like a free bus: while a
 * target stretches, interrupts run with SCL held low by this master too.
 */
static int scl_rise(struct mm_gpio_data *d, const struct mm_gpio_config *cfg, unsigned int *key)
{
	uint32_t tries = DIV_ROUND_UP(cfg->stretch_timeout_us, STRETCH_LOCKED_US);

	*key = irq_lock();
	scl_set(cfg, 1);
	while (!wait_scl_high(cfg, now() + us(d, STRETCH_LOCKED_US))) {
		if (tries-- == 0U) {
			irq_unlock(*key);
			return -EIO;
		}
		scl_fall(d, cfg);
		irq_unlock(*key);
		*key = irq_lock();
		scl_set(cfg, 1);
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
	return clock_bit(d, cfg, ack ? 0 : 1, false, &bit);
}

/* -EBUSY: someone else's transfer; -EIO: SDA held low with SCL high all along */
static int bus_free(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	uint32_t end = now() + us(d, cfg->bus_timeout_us);
	uint32_t idle_since = now();
	bool stuck = true;

	while (before(end)) {
		int scl = scl_get(cfg);
		int sda = sda_get(cfg);

		if (!(scl && sda)) {
			idle_since = now();
		} else if (!before(idle_since + us(d, cfg->bus_free_us))) {
			return 0;
		}
		stuck = stuck && scl && !sda;
	}
	return stuck ? -EIO : -EBUSY;
}

/* SCL high, SDA high -> SDA low -> SCL low */
static int start(struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	unsigned int key = irq_lock();

	if (!scl_get(cfg) || !sda_get(cfg)) {
		irq_unlock(key);
		return -EAGAIN;
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

/* The target gives its pins back when it is unregistered */
static int pins_take(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	int ret = d->target_cfg ? i2c_target_unregister(cfg->target, d->target_cfg) : 0;

	ret = ret ? ret : gpio_pin_configure_dt(&cfg->scl, PIN_FLAGS);
	return ret ? ret : gpio_pin_configure_dt(&cfg->sda, PIN_FLAGS);
}

/* The target takes the pins back before the GPIOs release them: after a STOP begun with
 * stop_begin(), releasing SDA ends it, and the target already sees the next START
 */
static void pins_to_target(const struct mm_gpio_config *cfg)
{
	if (cfg->pcfg != NULL) {
		(void)pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	}
}

static void pins_give(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	pins_to_target(cfg);
	if (d->target_cfg) {
		(void)target_reregister(cfg->target, d->target_cfg);
	}
	release(cfg);
}

static int msgs_run(struct mm_gpio_data *d, const struct mm_gpio_config *cfg, struct i2c_msg *msgs,
		    uint8_t num_msgs, uint16_t addr)
{
	bool prev_rd = false;
	int ret = start(d, cfg);

	for (uint8_t m = 0; m < num_msgs && ret == 0; m++) {
		bool rd = (msgs[m].flags & I2C_MSG_RW_MASK) == I2C_MSG_READ;
		bool addressed = m == 0 || (msgs[m].flags & I2C_MSG_RESTART) || rd != prev_rd;

		prev_rd = rd;
		if (m > 0 && addressed) {
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
					    (m + 1 == num_msgs || (msgs[m].flags & I2C_MSG_STOP));

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

	if (ret == 0 || ret == -ENXIO || ret == -EIO) {
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
	if (ret == -EIO) {
		LOG_WRN("SDA stuck low, recovering");
		ret = pins_take(d, cfg);
		ret = ret ? ret : recover(d, cfg);
		ret = ret ? ret : bus_free(d, cfg);
		if (ret == 0) {
			ret = msgs_run(d, cfg, msgs, num_msgs, addr);
		}
		pins_give(d, cfg);
	} else if (ret == 0) {
		ret = pins_take(d, cfg);
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

static int mm_gpio_target_register(const struct device *dev, struct i2c_target_config *tc)
{
	const struct mm_gpio_config *cfg = dev->config;
	struct mm_gpio_data *d = dev->data;
	int ret;

	k_mutex_lock(&d->lock, K_FOREVER);
	ret = d->target_cfg && d->target_cfg != tc ? -EBUSY : i2c_target_register(cfg->target, tc);
	if (ret == 0) {
		d->target_cfg = tc;
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
	ret = d->target_cfg != tc ? -EINVAL : i2c_target_unregister(cfg->target, tc);
	if (ret == 0) {
		d->target_cfg = NULL;
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
	ret = pins_take(d, cfg);
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
	pins_to_target(cfg);
	return mm_gpio_configure(dev, I2C_MODE_CONTROLLER |
					      I2C_SPEED_SET(cfg->bitrate > I2C_BITRATE_STANDARD
								    ? I2C_SPEED_FAST
								    : I2C_SPEED_STANDARD));
}

/* clang-format off */
#define MM_GPIO_INST(n)                                                                            \
	IF_ENABLED(DT_INST_PINCTRL_HAS_NAME(n, default), (PINCTRL_DT_INST_DEFINE(n);))             \
	static struct mm_gpio_data mm_gpio_data_##n;                                               \
	static const struct mm_gpio_config mm_gpio_config_##n = {                                  \
		.target = DEVICE_DT_GET(DT_INST_PHANDLE(n, target)),                               \
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
