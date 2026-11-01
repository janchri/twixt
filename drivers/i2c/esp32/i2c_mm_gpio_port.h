/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TWIXT_ESP32_GPIO_PORT_H_
#define TWIXT_ESP32_GPIO_PORT_H_

#include <esp_cpu.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>
#ifdef CONFIG_I2C_MM_ESP32_TARGET
#include "i2c_mm_esp32_target.h"
#endif

static inline void pin_set(const struct gpio_dt_spec *g, int v)
{
	gpio_ll_set_level(&GPIO, g->pin, v);
}

static inline int pin_get(const struct gpio_dt_spec *g)
{
	return gpio_ll_get_level(&GPIO, g->pin);
}

static inline uint32_t now(void)
{
	return esp_cpu_get_cycle_count();
}

static inline uint32_t counter_init(void)
{
	return CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U;
}

#ifdef CONFIG_I2C_MM_ESP32_TARGET
/* Optional target handoff contract:
 * matches() selects only target devices owned by this backend.
 * take() borrows pins without unregistering; on error it leaves the target intact.
 * give() restores target routing after the controller releases the lines.
 * The backend owns its interrupts throughout. Ports without this define retain
 * the existing unregister/restore path, including target-restore reporting.
 */
#define MM_GPIO_TARGET_HANDOFF 1
static inline bool target_handoff_matches(const struct device *dev)
{
	return i2c_mm_esp32_target_matches(dev);
}

static inline int target_handoff_take(const struct device *dev)
{
	return i2c_mm_esp32_target_take(dev);
}

static inline void target_handoff_give(const struct device *dev)
{
	i2c_mm_esp32_target_give(dev);
}
#endif /* CONFIG_I2C_MM_ESP32_TARGET */
#endif
