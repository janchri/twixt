/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The port of twixt,i2c-mm-gpio on the fake bus of gpio_bus.c.
 */

#ifndef TESTS_LINK_I2C_MM_GPIO_PORT_H_
#define TESTS_LINK_I2C_MM_GPIO_PORT_H_

#include <zephyr/drivers/gpio.h>

void fake_pin_set(int pin, int v);
int fake_pin_get(int pin);
uint32_t fake_now(void);

static inline void pin_set(const struct gpio_dt_spec *g, int v)
{
	fake_pin_set(g->pin, v);
}

static inline int pin_get(const struct gpio_dt_spec *g)
{
	return fake_pin_get(g->pin);
}

static inline uint32_t now(void)
{
	return fake_now();
}

static inline uint32_t counter_init(void)
{
	return 1;
}

#endif /* TESTS_LINK_I2C_MM_GPIO_PORT_H_ */
