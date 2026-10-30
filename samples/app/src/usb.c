/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * On a board with USB CDC-ACM ports (the Raspberry Pi Pico): brings up the USB device with all of
 * them, the log and MCUmgr, before main(). Zephyr's CDC_ACM_SERIAL_INITIALIZE_AT_BOOT would
 * only bring up the first.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>

/* Zephyr's VID and a PID for testing; a product needs its own */
USBD_DEVICE_DEFINE(twixt_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x2fe3, 0x0004);
USBD_DESC_LANG_DEFINE(twixt_lang);
USBD_DESC_MANUFACTURER_DEFINE(twixt_mfr, "twixt");
USBD_DESC_PRODUCT_DEFINE(twixt_product, "twixt pico");
USBD_DESC_CONFIG_DEFINE(twixt_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(twixt_config, 0, 125, &twixt_cfg_desc);

static int usb_init(void)
{
	struct usbd_desc_node *const descs[] = {&twixt_lang, &twixt_mfr, &twixt_product};
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(descs); i++) {
		err = usbd_add_descriptor(&twixt_usbd, descs[i]);
		if (err) {
			return err;
		}
	}
	err = usbd_add_configuration(&twixt_usbd, USBD_SPEED_FS, &twixt_config);
	if (err) {
		return err;
	}
	err = usbd_register_all_classes(&twixt_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		return err;
	}
	/* CDC-ACM has more than one interface per port: interface association */
	usbd_device_set_code_triple(&twixt_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);

	err = usbd_init(&twixt_usbd);
	if (err) {
		return err;
	}
	return usbd_enable(&twixt_usbd);
}

SYS_INIT(usb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
