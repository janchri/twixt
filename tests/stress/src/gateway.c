/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The gateway: bridges the boards' shells to the host, and has its own shell, the prompt its
 * name: gw@70:~$ (gateway.overlay). With USB CDC-ACM ports (Raspberry Pi Pico), it brings up
 * the USB device with all of them.
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell_uart.h>

#include <twixt/twixt.h>

LOG_MODULE_REGISTER(gateway, LOG_LEVEL_INF);

#if DT_HAS_COMPAT_STATUS_OKAY(zephyr_cdc_acm_uart)

#include <zephyr/usb/usbd.h>

/* Zephyr's VID and a PID for testing; a product needs its own */
USBD_DEVICE_DEFINE(gw_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x2fe3, 0x0004);
USBD_DESC_LANG_DEFINE(gw_lang);
USBD_DESC_MANUFACTURER_DEFINE(gw_mfr, "twixt");
USBD_DESC_PRODUCT_DEFINE(gw_product, "twixt pico");
USBD_DESC_CONFIG_DEFINE(gw_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(gw_config, 0, 125, &gw_cfg_desc);

static int usb_init(void)
{
	struct usbd_desc_node *const descs[] = {&gw_lang, &gw_mfr, &gw_product};
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(descs); i++) {
		err = usbd_add_descriptor(&gw_usbd, descs[i]);
		if (err) {
			return err;
		}
	}
	err = usbd_add_configuration(&gw_usbd, USBD_SPEED_FS, &gw_config);
	if (err) {
		return err;
	}
	err = usbd_register_all_classes(&gw_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		return err;
	}
	/* CDC-ACM has more than one interface per port: interface association */
	usbd_device_set_code_triple(&gw_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);

	err = usbd_init(&gw_usbd);
	if (err) {
		return err;
	}
	return usbd_enable(&gw_usbd);
}

#endif

int main(void)
{
	static char prompt[32];

#if DT_HAS_COMPAT_STATUS_OKAY(zephyr_cdc_acm_uart)
	int err = usb_init();

	if (err) {
		LOG_ERR("USB: %d", err);
	}
#endif
	snprintf(prompt, sizeof(prompt), "%s:~$ ", twixt_self_name());
	shell_prompt_change(shell_backend_uart_get_ptr(), prompt);

	LOG_INF("%s at 0x%02x: gateway", twixt_self_name(), twixt_self_addr());
	return 0;
}
