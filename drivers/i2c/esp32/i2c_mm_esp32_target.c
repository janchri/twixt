/* SPDX-License-Identifier: Apache-2.0 */
/* Classic ESP32 target: GPIO stretching before the repeated-START read address.
 * The hardware stays in target mode even while the software controller owns SDA.
 * No IDF driver state or FreeRTOS APIs are used.
 */
#define DT_DRV_COMPAT twixt_esp32_i2c_target

#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_rom_gpio.h>
#include <hal/gpio_ll.h>
#include <hal/i2c_ll.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_struct.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/interrupt_controller/intc_esp32.h>
#include <zephyr/kernel.h>
#include "i2c_mm_esp32_target.h"

#define RX_INTR   I2C_RXFIFO_FULL_INT_ENA_M
#define TX_INTR   I2C_TXFIFO_EMPTY_INT_ENA_M
#define END_INTR  I2C_TRANS_COMPLETE_INT_ENA_M
#define PIN_FLAGS (GPIO_INPUT | GPIO_OUTPUT | GPIO_OUTPUT_INIT_HIGH | GPIO_OPEN_DRAIN)
#define CYCLES_US (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U)

/* Buffer-mode callbacks are never called here, and twixt,i2c-mm-gpio installs them in that mode */
BUILD_ASSERT(!IS_ENABLED(CONFIG_I2C_TARGET_BUFFER_MODE),
	     "The ESP32 target needs CONFIG_I2C_TARGET_BUFFER_MODE=n");

struct target_config {
	const struct device *clock;
	clock_control_subsys_t clock_subsys;
	struct gpio_dt_spec scl, sda;
	int port, irq;
};

struct target_data {
	i2c_dev_t *hw;
	struct i2c_target_config *target;
	struct gpio_callback edges;
	const struct device *dev;
	bool writing, reading, armed, held, controller;
	uint32_t rise;
	uint32_t edge; /* the last SCL or SDA edge the target saw */
};

BUILD_ASSERT(DT_IRQ(DT_NODELABEL(gpio0), priority) == 3,
	     "ESP32 target requires GPIO interrupt priority 3, matching its I2C ISR");

static const struct i2c_driver_api target_api;

bool i2c_mm_esp32_target_matches(const struct device *dev)
{
	return dev->api == &target_api;
}

static void IRAM_ATTR drain_rx(const struct device *dev)
{
	struct target_data *d = dev->data;
	const struct i2c_target_callbacks *cb = d->target->callbacks;
	uint32_t count;

	i2c_ll_get_rxfifo_cnt(d->hw, &count);
	while (count--) {
		uint8_t byte;

		i2c_ll_read_rxfifo(d->hw, &byte, 1);
		if (!d->writing) {
			d->writing = true;
			if (cb->write_requested) {
				cb->write_requested(d->target);
			}
		}
		if (cb->write_received) {
			cb->write_received(d->target, byte);
		}
	}
}

static void IRAM_ATTR fill_tx(const struct device *dev)
{
	struct target_data *d = dev->data;
	const struct i2c_target_callbacks *cb = d->target->callbacks;
	uint32_t space;

	i2c_ll_get_txfifo_len(d->hw, &space);
	while (space--) {
		uint8_t byte = 0xff;

		if (!d->reading) {
			d->reading = true;
			if (cb->read_requested) {
				cb->read_requested(d->target, &byte);
			}
		} else if (cb->read_processed) {
			cb->read_processed(d->target, &byte);
		}
		i2c_ll_write_txfifo(d->hw, &byte, 1);
	}
}

static void IRAM_ATTR peripheral_isr(void *arg)
{
	const struct device *dev = arg;
	struct target_data *d = dev->data;
	uint32_t status = d->hw->int_status.val;

	i2c_ll_clear_intr_mask(d->hw, status);
	if (!d->target || d->controller) {
		return;
	}
	if (status & RX_INTR) {
		drain_rx(dev);
	}
	if (status & END_INTR) {
		drain_rx(dev);
		i2c_ll_disable_intr_mask(d->hw, TX_INTR);
		i2c_ll_txfifo_rst(d->hw);
		if (d->target->callbacks->stop) {
			d->target->callbacks->stop(d->target);
		}
		d->writing = false;
		d->reading = false;
		d->armed = false;
	} else if ((status & TX_INTR) && d->reading) {
		fill_tx(dev);
	}
}

/* GPIO and I2C interrupts must share a core and priority. No thread is required:
 * Twixt's target callbacks already run in interrupt context on the other ports.
 */
static void IRAM_ATTR boundary(const struct device *gpio, struct gpio_callback *cb,
			       gpio_port_pins_t pending)
{
	struct target_data *d = CONTAINER_OF(cb, struct target_data, edges);
	const struct device *dev = d->dev;
	const struct target_config *cfg = dev->config;
	int scl = gpio_ll_get_level(&GPIO, cfg->scl.pin);

	ARG_UNUSED(gpio);
	d->edge = esp_cpu_get_cycle_count();
	if (!d->target || d->controller || d->held) {
		return;
	}
	/* Once armed, clamp before any FIFO or SDA reads: the next rise starts
	 * clocking the read address. Most edges need only the SCL sample below.
	 */
	if (d->armed && (pending & BIT(cfg->scl.pin))) {
		if (scl) {
			/* The first fall escaped us and the read address is already
			 * clocking. Never clamp at a later fall inside that address.
			 */
			d->armed = false;
			return;
		}
		gpio_ll_set_level(&GPIO, cfg->scl.pin, 0);
		d->held = true;
		d->armed = false;
		drain_rx(dev);
		i2c_ll_disable_intr_mask(d->hw, TX_INTR);
		i2c_ll_txfifo_rst(d->hw);
		d->reading = false;
		fill_tx(dev);
		i2c_ll_clear_intr_mask(d->hw, TX_INTR);
		i2c_ll_enable_intr_mask(d->hw, TX_INTR);
		d->held = false;
		gpio_ll_set_level(&GPIO, cfg->scl.pin, 1);
		return;
	}
	if ((pending & BIT(cfg->scl.pin)) && scl) {
		d->rise = esp_cpu_get_cycle_count();
	}
	if ((pending & BIT(cfg->sda.pin)) && !(pending & BIT(cfg->scl.pin)) && scl &&
	    !gpio_ll_get_level(&GPIO, cfg->sda.pin)) {
		uint32_t late_edges;
		uint32_t count = 0;

		/* Zephyr collected pending before calling us. Sample levels first,
		 * then check for an SCL edge since that snapshot, as in the raw
		 * ISR: a data edge delayed across a clock rise is not a START.
		 * Do not clear these flags; the GPIO driver owns their dispatch.
		 */
		gpio_ll_get_intr_status(&GPIO, arch_curr_cpu()->id, &late_edges);
		if ((late_edges & BIT(cfg->scl.pin)) ||
		    esp_cpu_get_cycle_count() - d->rise < 3 * CYCLES_US) {
			return;
		}
		if (!d->writing) {
			i2c_ll_get_rxfifo_cnt(d->hw, &count);
		}
		d->armed = d->writing || count;
	}
}

/* Routes SDA back to the peripheral and listens for the repeated START again */
static void target_arm(const struct target_config *cfg, struct target_data *d)
{
	esp_rom_gpio_connect_out_signal(cfg->sda.pin,
					(cfg->port ? I2CEXT1_SDA_OUT_IDX : I2CEXT0_SDA_OUT_IDX),
					false, false);
	d->rise = d->edge = esp_cpu_get_cycle_count();
	gpio_pin_interrupt_configure_dt(&cfg->scl, GPIO_INT_EDGE_BOTH);
	gpio_pin_interrupt_configure_dt(&cfg->sda, GPIO_INT_EDGE_FALLING);
}

/* A peer's frame is in flight, as far as the peripheral or this driver can tell. Short writes
 * set no flag before their STOP, so the peripheral's bus-busy bit and RX FIFO count too. The
 * state is stale once both lines have been high, with no edge, for idle_us: the controller's
 * bus_free() takes such a bus as free anyway, and a peer that reset mid-frame never sends STOP.
 */
static bool peer_frame(const struct target_config *cfg, struct target_data *d, uint32_t idle_us)
{
	uint32_t count;

	i2c_ll_get_rxfifo_cnt(d->hw, &count);
	if (!(d->armed || d->writing || d->reading || count || i2c_ll_is_bus_busy(d->hw))) {
		return false;
	}
	return !gpio_ll_get_level(&GPIO, cfg->scl.pin) || !gpio_ll_get_level(&GPIO, cfg->sda.pin) ||
	       esp_cpu_get_cycle_count() - d->edge < idle_us * CYCLES_US;
}

int i2c_mm_esp32_target_take(const struct device *dev, uint32_t idle_us, bool force)
{
	const struct target_config *cfg = dev->config;
	struct target_data *d = dev->data;
	unsigned int key = irq_lock();

	if (d->target && !force && peer_frame(cfg, d, idle_us)) {
		irq_unlock(key);
		return -EBUSY;
	}
	d->controller = true;
	gpio_pin_interrupt_configure_dt(&cfg->scl, GPIO_INT_DISABLE);
	gpio_pin_interrupt_configure_dt(&cfg->sda, GPIO_INT_DISABLE);
	esp_rom_gpio_connect_out_signal(cfg->sda.pin, SIG_GPIO_OUT_IDX, false, false);
	if (d->writing || d->reading || d->armed) {
		/* A dead peer's frame: no STOP will end it, so drop it here */
		i2c_ll_disable_intr_mask(d->hw, TX_INTR);
		i2c_ll_txfifo_rst(d->hw);
		i2c_ll_rxfifo_rst(d->hw);
		d->writing = d->reading = d->armed = false;
	}
	irq_unlock(key);
	return 0;
}

void i2c_mm_esp32_target_give(const struct device *dev)
{
	const struct target_config *cfg = dev->config;
	struct target_data *d = dev->data;
	unsigned int key = irq_lock();

	d->controller = false;
	if (d->target) {
		/* Our frame ended with a STOP; still busy: a peer STARTed since, keep its data */
		if (!i2c_ll_is_bus_busy(d->hw)) {
			i2c_ll_rxfifo_rst(d->hw);
			i2c_ll_clear_intr_mask(d->hw, UINT32_MAX);
		}
		target_arm(cfg, d);
	}
	irq_unlock(key);
}

static int target_register(const struct device *dev, struct i2c_target_config *target)
{
	const struct target_config *cfg = dev->config;
	struct target_data *d = dev->data;
	unsigned int key;

	if (!target || !target->callbacks || target->address > 0x7f ||
	    (target->flags & I2C_TARGET_FLAGS_ADDR_10_BITS)) {
		return -EINVAL;
	}
	key = irq_lock();
	if (d->target) {
		irq_unlock(key);
		return -EBUSY;
	}
	d->target = target;
	d->writing = d->reading = d->armed = false;
	i2c_ll_set_mode(d->hw, I2C_BUS_MODE_SLAVE);
	i2c_ll_enable_pins_open_drain(d->hw, true);
	i2c_ll_enable_arbitration(d->hw, false);
	i2c_ll_set_data_mode(d->hw, I2C_DATA_MODE_MSB_FIRST, I2C_DATA_MODE_MSB_FIRST);
	i2c_ll_txfifo_rst(d->hw);
	i2c_ll_rxfifo_rst(d->hw);
	i2c_ll_enable_fifo_mode(d->hw, true);
	i2c_ll_set_slave_addr(d->hw, target->address, false);
	i2c_ll_set_rxfifo_full_thr(d->hw, 16);
	i2c_ll_set_txfifo_empty_thr(d->hw, 8);
	i2c_ll_set_tout(d->hw, I2C_LL_MAX_TIMEOUT);
	i2c_ll_set_sda_timing(d->hw, 10, 10);
	i2c_ll_clear_intr_mask(d->hw, UINT32_MAX);
	i2c_ll_enable_intr_mask(d->hw, RX_INTR | END_INTR);
	target_arm(cfg, d);
	irq_unlock(key);
	return 0;
}

static int target_unregister(const struct device *dev, struct i2c_target_config *target)
{
	const struct target_config *cfg = dev->config;
	struct target_data *d = dev->data;
	unsigned int key = irq_lock();

	if (!target || !d->target) {
		irq_unlock(key);
		return -EINVAL;
	}
	if (d->target != target || d->controller || d->held) {
		irq_unlock(key);
		return -EBUSY;
	}
	gpio_pin_interrupt_configure_dt(&cfg->scl, GPIO_INT_DISABLE);
	gpio_pin_interrupt_configure_dt(&cfg->sda, GPIO_INT_DISABLE);
	i2c_ll_disable_intr_mask(d->hw, UINT32_MAX);
	d->target = NULL;
	d->writing = d->reading = d->armed = false;
	esp_rom_gpio_connect_out_signal(cfg->sda.pin, SIG_GPIO_OUT_IDX, false, false);
	irq_unlock(key);
	return 0;
}

/* Target only; the I2C shell lists this device too, and i2c_transfer() has no NULL check */
static int target_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
			   uint16_t addr)
{
	return -ENOTSUP;
}

static DEVICE_API(i2c, target_api) = {
	.transfer = target_transfer,
	.target_register = target_register,
	.target_unregister = target_unregister,
};

static int target_init(const struct device *dev)
{
	const struct target_config *cfg = dev->config;
	struct target_data *d = dev->data;
	int ret;

	if (!device_is_ready(cfg->clock) || !gpio_is_ready_dt(&cfg->scl) ||
	    cfg->scl.port != DEVICE_DT_GET(DT_NODELABEL(gpio0)) || cfg->scl.port != cfg->sda.port ||
	    cfg->scl.pin >= 32 || cfg->sda.pin >= 32 || cfg->scl.pin == cfg->sda.pin) {
		return -ENODEV;
	}
	d->dev = dev;
	ret = clock_control_on(cfg->clock, cfg->clock_subsys);
	if (ret) {
		return ret;
	}
	d->hw = I2C_LL_GET_HW(cfg->port);
	i2c_ll_enable_controller_clock(d->hw, true);
	i2c_ll_disable_intr_mask(d->hw, UINT32_MAX);
	ret = gpio_pin_configure_dt(&cfg->scl, PIN_FLAGS);
	ret = ret ? ret : gpio_pin_configure_dt(&cfg->sda, PIN_FLAGS);
	if (ret) {
		return ret;
	}
	esp_rom_gpio_connect_in_signal(
		cfg->scl.pin, (cfg->port ? I2CEXT1_SCL_IN_IDX : I2CEXT0_SCL_IN_IDX), false);
	esp_rom_gpio_connect_in_signal(
		cfg->sda.pin, (cfg->port ? I2CEXT1_SDA_IN_IDX : I2CEXT0_SDA_IN_IDX), false);
	gpio_init_callback(&d->edges, boundary, BIT(cfg->scl.pin) | BIT(cfg->sda.pin));
	ret = gpio_add_callback(cfg->scl.port, &d->edges);
	return ret ? ret
		   : esp_intr_alloc(cfg->irq, ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM,
				    peripheral_isr, (void *)dev, NULL);
}

#define TARGET_DEFINE(n)                                                                           \
	static struct target_data target_data_##n;                                                 \
	static const struct target_config target_config_##n = {                                    \
		.clock = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                    \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, offset),            \
		.scl = GPIO_DT_SPEC_INST_GET(n, scl_gpios),                                        \
		.sda = GPIO_DT_SPEC_INST_GET(n, sda_gpios),                                        \
		.port = DT_INST_PROP(n, port),                                                     \
		.irq = DT_INST_IRQ(n, irq),                                                        \
	};                                                                                         \
	I2C_DEVICE_DT_INST_DEFINE(n, target_init, NULL, &target_data_##n, &target_config_##n,      \
				  POST_KERNEL, CONFIG_I2C_INIT_PRIORITY, &target_api);

DT_INST_FOREACH_STATUS_OKAY(TARGET_DEFINE)
