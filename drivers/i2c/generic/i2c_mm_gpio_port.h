/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pins and counter of twixt,i2c-mm-gpio: the GPIO API and the timing functions.
 */

#ifndef DRIVERS_I2C_GENERIC_I2C_MM_GPIO_PORT_H_
#define DRIVERS_I2C_GENERIC_I2C_MM_GPIO_PORT_H_

#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/timing/timing.h>

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

/* Returns the counter's cycles per us */
static inline uint32_t counter_init(void)
{
	timing_init();
	timing_start();
	return MAX(timing_freq_get() / USEC_PER_SEC, 1);
}

#endif /* DRIVERS_I2C_GENERIC_I2C_MM_GPIO_PORT_H_ */
