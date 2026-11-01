/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pins and counter of twixt,i2c-mm-gpio on the RP2040: the SIO registers and the 1 MHz timer,
 * since k_cycle_get_32() and the GPIO API take about 1 us per call there.
 */

#ifndef DRIVERS_I2C_RP2040_I2C_MM_GPIO_PORT_H_
#define DRIVERS_I2C_RP2040_I2C_MM_GPIO_PORT_H_

#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>

#include <hardware/gpio.h>
#include <hardware/timer.h>

/* Open drain: the output is 0, the output enable pulls the line low */
static inline void pin_set(const struct gpio_dt_spec *g, int v)
{
	gpio_set_dir(g->pin, v ? GPIO_IN : GPIO_OUT);
}

static inline int pin_get(const struct gpio_dt_spec *g)
{
	return gpio_get(g->pin);
}

static inline uint32_t now(void)
{
	return time_us_32();
}

/* Returns the counter's cycles per us */
static inline uint32_t counter_init(void)
{
	return 1;
}

#endif /* DRIVERS_I2C_RP2040_I2C_MM_GPIO_PORT_H_ */
