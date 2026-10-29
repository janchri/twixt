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
 * Times are counted in reads of SCL, calibrated against the system clock at
 * init: short busy waits are far off on some SoCs. The calibration also times
 * a bit without waits, the cost of the GPIO calls, and takes it off the waits:
 * a third off the low phase, two thirds off the high phase, which releases SCL,
 * waits for it and samples SDA.
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

#include "target_reregister.h"

#define STRETCH_LOCKED_US 100
#define RECOVERY_CLOCKS   9
#define CALIBRATE_MS      10
#define LOW_SHARE         55 /* % of the period, like the minimum 4.7 us low to 4.0 us high */
#define START_STOP_US     5  /* > tSU;STA 4.7 us, tHD;STA and tSU;STO 4.0 us */

#define US_TO_POLLS(d, us) DIV_ROUND_UP((uint64_t)(us) * (d)->polls_per_ms, USEC_PER_MSEC)

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
	uint32_t polls_per_ms;
	uint32_t bit_cost; /* of clock_bit() without waits */
	uint32_t t_low; /* in SCL reads, like all times below */
	uint32_t t_high;
};

static inline void scl_set(const struct mm_gpio_config *cfg, int v)
{
	gpio_pin_set_raw(cfg->scl.port, cfg->scl.pin, v);
}

static inline void sda_set(const struct mm_gpio_config *cfg, int v)
{
	gpio_pin_set_raw(cfg->sda.port, cfg->sda.pin, v);
}

static inline int scl_get(const struct mm_gpio_config *cfg)
{
	return gpio_pin_get_raw(cfg->scl.port, cfg->scl.pin);
}

static inline int sda_get(const struct mm_gpio_config *cfg)
{
	return gpio_pin_get_raw(cfg->sda.port, cfg->sda.pin);
}

static void hold(const struct mm_gpio_config *cfg, uint32_t polls)
{
	while (polls-- > 0) {
		(void)scl_get(cfg);
	}
}

static bool wait_scl_high(const struct mm_gpio_config *cfg, uint32_t polls)
{
	for (uint32_t i = 0; !scl_get(cfg); i++) {
		if (i >= polls) {
			return false;
		}
	}
	return true;
}

/* Releases SCL, waits out stretching and returns with interrupts locked and SCL high. SCL is
 * high only with interrupts locked, else a preempted frame looks like a free bus: while a
 * target stretches, interrupts run with SCL held low by this master too.
 */
static int scl_rise(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg,
		    unsigned int *key)
{
	uint32_t tries = DIV_ROUND_UP(cfg->stretch_timeout_us, STRETCH_LOCKED_US);

	*key = irq_lock();
	scl_set(cfg, 1);
	while (!wait_scl_high(cfg, US_TO_POLLS(d, STRETCH_LOCKED_US))) {
		if (tries-- == 0U) {
			irq_unlock(*key);
			return -EIO;
		}
		scl_set(cfg, 0);
		irq_unlock(*key);
		*key = irq_lock();
		scl_set(cfg, 1);
	}
	return 0;
}

/* One bit; *bit is what was on SDA. With arb, a 1 that reads 0 releases both lines. */
static int clock_bit(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg, int out,
		     bool arb, int *bit)
{
	unsigned int key;
	int ret;

	sda_set(cfg, out);
	hold(cfg, d->t_low);
	ret = scl_rise(d, cfg, &key);
	if (ret) {
		sda_set(cfg, 1);
		return ret;
	}
	*bit = sda_get(cfg);
	if (arb && out && !*bit) {
		irq_unlock(key);
		return -EAGAIN;
	}
	for (uint32_t i = 0; i < d->t_high && scl_get(cfg); i++) {
	}
	scl_set(cfg, 0);
	irq_unlock(key);
	return 0;
}

/* 0: ACK, 1: NACK */
static int write_byte(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg, uint8_t val)
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

static int read_byte(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg, uint8_t *val,
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
	uint32_t idle_polls = US_TO_POLLS(d, cfg->bus_free_us) / 2; /* both lines: two polls */
	uint32_t idle = 0;
	bool stuck = true;

	for (uint32_t i = 0; i < US_TO_POLLS(d, cfg->bus_timeout_us) / 2; i++) {
		int scl = scl_get(cfg);
		int sda = sda_get(cfg);

		idle = scl && sda ? idle + 1 : 0;
		if (idle >= idle_polls) {
			return 0;
		}
		stuck = stuck && scl && !sda;
	}
	return stuck ? -EIO : -EBUSY;
}

/* SCL high, SDA high -> SDA low -> SCL low */
static int start(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	unsigned int key = irq_lock();

	if (!scl_get(cfg) || !sda_get(cfg)) {
		irq_unlock(key);
		return -EAGAIN;
	}
	sda_set(cfg, 0);
	hold(cfg, US_TO_POLLS(d, START_STOP_US));
	scl_set(cfg, 0);
	irq_unlock(key);
	return 0;
}

static int restart(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	unsigned int key;
	int ret;

	sda_set(cfg, 1);
	hold(cfg, d->t_low);
	ret = scl_rise(d, cfg, &key);
	if (ret) {
		return ret;
	}
	hold(cfg, US_TO_POLLS(d, START_STOP_US));
	sda_set(cfg, 0);
	hold(cfg, US_TO_POLLS(d, START_STOP_US));
	scl_set(cfg, 0);
	irq_unlock(key);
	return 0;
}

/* A STOP up to its last edge: SCL high, SDA low */
static void stop_begin(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	unsigned int key;

	sda_set(cfg, 0);
	hold(cfg, d->t_low);
	if (scl_rise(d, cfg, &key) == 0) {
		irq_unlock(key);
	}
	hold(cfg, US_TO_POLLS(d, START_STOP_US));
}

static void stop(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	stop_begin(d, cfg);
	sda_set(cfg, 1);
	hold(cfg, d->t_low);
}

static void release(const struct mm_gpio_config *cfg)
{
	sda_set(cfg, 1);
	scl_set(cfg, 1);
}

static int recover(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	unsigned int key;

	sda_set(cfg, 1);
	for (int i = 0; i < RECOVERY_CLOCKS && !sda_get(cfg); i++) {
		scl_set(cfg, 0);
		hold(cfg, d->t_low);
		if (scl_rise(d, cfg, &key)) {
			return -EIO;
		}
		irq_unlock(key);
		hold(cfg, d->t_high);
	}
	scl_set(cfg, 0);
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

static int msgs_run(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg,
		    struct i2c_msg *msgs, uint8_t num_msgs, uint16_t addr)
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
	uint32_t period, low, high, cost_low, cost_high;
	uint32_t rate;

	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		rate = I2C_BITRATE_STANDARD;
		break;
	case I2C_SPEED_FAST:
		rate = I2C_BITRATE_FAST;
		break;
	default:
		return -ENOTSUP;
	}
	if (!(dev_config & I2C_MODE_CONTROLLER) || (dev_config & I2C_ADDR_10_BITS)) {
		return -ENOTSUP;
	}

	k_mutex_lock(&d->lock, K_FOREVER);
	period = DIV_ROUND_UP(MSEC_PER_SEC * d->polls_per_ms, rate);
	low = period * LOW_SHARE / 100;
	high = period - low;
	cost_low = d->bit_cost / 3;
	cost_high = d->bit_cost - cost_low;
	d->t_low = low > cost_low ? low - cost_low : 0;
	d->t_high = high > cost_high ? high - cost_high : 0;
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

static void run_hold(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg, uint32_t n)
{
	hold(cfg, n);
}

static void run_bits(const struct mm_gpio_data *d, const struct mm_gpio_config *cfg, uint32_t n)
{
	int bit;

	while (n-- > 0) {
		(void)clock_bit(d, cfg, 1, false, &bit);
	}
}

/* Runs of run() per millisecond, doubled until they last CALIBRATE_MS; the cycle counter runs
 * with interrupts locked
 */
static uint32_t runs_per_ms(void (*run)(const struct mm_gpio_data *, const struct mm_gpio_config *,
				       uint32_t),
			    const struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	uint32_t hz = sys_clock_hw_cycles_per_sec();
	uint32_t span = hz / (MSEC_PER_SEC / CALIBRATE_MS);
	uint32_t n = 16;
	uint32_t cycles;

	do {
		unsigned int key = irq_lock();
		uint32_t start = k_cycle_get_32();

		while (k_cycle_get_32() == start) {
		}
		start = k_cycle_get_32();
		run(d, cfg, n);
		cycles = k_cycle_get_32() - start;
		irq_unlock(key);
		n *= 2;
	} while (cycles < span);

	return MAX((uint32_t)((uint64_t)n / 2 * hz / ((uint64_t)cycles * MSEC_PER_SEC)), 1);
}

/* Before the target block has the pins, as inputs: the bits stay off the bus. A bus that is
 * not idle stretches the bits; then nothing is taken off the waits.
 */
static void calibrate(struct mm_gpio_data *d, const struct mm_gpio_config *cfg)
{
	uint32_t bits_per_ms;

	(void)gpio_pin_configure_dt(&cfg->scl, GPIO_INPUT);
	(void)gpio_pin_configure_dt(&cfg->sda, GPIO_INPUT);
	d->polls_per_ms = runs_per_ms(run_hold, d, cfg);
	d->t_low = 0;
	d->t_high = 0;
	d->bit_cost = 0;
	if (scl_get(cfg) && sda_get(cfg)) {
		bits_per_ms = runs_per_ms(run_bits, d, cfg);
		d->bit_cost = DIV_ROUND_UP(d->polls_per_ms, bits_per_ms);
	}
	if (d->bit_cost > d->polls_per_ms / 100) {
		LOG_WRN("bus not idle at init, bit timing not corrected");
		d->bit_cost = 0;
	}
	LOG_INF("%u SCL reads per ms, a bit costs %u", d->polls_per_ms, d->bit_cost);
}

static int mm_gpio_init(const struct device *dev)
{
	const struct mm_gpio_config *cfg = dev->config;
	struct mm_gpio_data *d = dev->data;

	if (!device_is_ready(cfg->target) || !gpio_is_ready_dt(&cfg->scl) ||
	    !gpio_is_ready_dt(&cfg->sda)) {
		return -ENODEV;
	}
	k_mutex_init(&d->lock);
	calibrate(d, cfg);
	pins_to_target(cfg);
	return mm_gpio_configure(dev, I2C_MODE_CONTROLLER |
					      I2C_SPEED_SET(cfg->bitrate > I2C_BITRATE_STANDARD
								    ? I2C_SPEED_FAST
								    : I2C_SPEED_STANDARD));
}

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

DT_INST_FOREACH_STATUS_OKAY(MM_GPIO_INST)
