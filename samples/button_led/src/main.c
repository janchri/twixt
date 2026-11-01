/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A button on one MCU toggles the LED of the other: a service of your own, in a few lines. An MCU
 * without a button (the Raspberry Pi Pico) only serves.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <twixt/twixt.h>

LOG_MODULE_REGISTER(button_led_sample, LOG_LEVEL_INF);

#define TOGGLE_SERVICE TWIXT_SERVICE_APP
#define DEBOUNCE       K_MSEC(30)

#define PICO DT_REG_ADDR(DT_NODELABEL(mcu_pico))
#define NRF  DT_REG_ADDR(DT_NODELABEL(mcu_nrf))
#define PEER (twixt_self_addr() == PICO ? NRF : PICO)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Runs in the I2C target interrupt: short, no waiting */
static int toggle(const struct twixt_req *req, uint8_t *rsp_data)
{
	return gpio_pin_toggle_dt(&led);
}

TWIXT_SERVICE_DEFINE(toggle_service, TOGGLE_SERVICE, toggle);

#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(sw0))
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_cb;

static void send_toggle(struct k_work *work)
{
	int ret;

	if (gpio_pin_get_dt(&button) <= 0) {
		return;
	}
	ret = twixt_request(PEER, TOGGLE_SERVICE, NULL, 0, NULL, 0);
	LOG_INF("toggle on 0x%02x: %d", PEER, ret);
}

static K_WORK_DELAYABLE_DEFINE(toggle_work, send_toggle);

static void button_changed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_work_reschedule(&toggle_work, DEBOUNCE);
}

static int button_start(void)
{
	int ret = gpio_pin_configure_dt(&button, GPIO_INPUT);

	ret = ret ? ret : gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret == 0) {
		gpio_init_callback(&button_cb, button_changed, BIT(button.pin));
		ret = gpio_add_callback_dt(&button, &button_cb);
	}
	return ret;
}
#else
static int button_start(void)
{
	return 0;
}
#endif

int main(void)
{
	int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	ret = ret ? ret : button_start();
	if (ret) {
		LOG_ERR("cannot set up the pins: %d", ret);
		return 0;
	}
	LOG_INF("%s at 0x%02x", twixt_self_name(), twixt_self_addr());
	return 0;
}
