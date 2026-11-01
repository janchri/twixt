Button and LED
==============

Press button 1 on the DK and the LED on the Pico toggles over the bus. The Pico has no user button, so it only serves.

.. code-block:: c

   static int toggle(const struct twixt_req *req, uint8_t *rsp_data)
   {
   	return gpio_pin_toggle_dt(&led);
   }

   TWIXT_SERVICE_DEFINE(toggle_service, TWIXT_SERVICE_APP, toggle);

   twixt_request(peer, TWIXT_SERVICE_APP, NULL, 0, NULL, 0);

Hardware
--------

Real boards only. Set up the boards as in the `main README <../../README.rst#on-real-boards>`_. Twister only builds samples, so flash them by hand.

.. code-block:: console

   west twister -p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T modules/lib/twixt/samples/button_led

   west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-button_led-pico modules/lib/twixt/samples/button_led
   west build -b nrf7002dk/nrf5340/cpuapp -d build-button_led-nrf modules/lib/twixt/samples/button_led
   west flash -d build-button_led-nrf
   smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if02 --timeout 60 upgrade --confirm build-button_led-pico/button_led/zephyr/zephyr.signed.bin

Each press logs ``toggle on 0x10: 0`` on the DK's VCOM.
