A service of your own
=====================

Both MCUs serve ``add``, and each asks the other for a sum. Ids from ``TWIXT_SERVICE_APP`` up are the application's, the same on every MCU of the bus.

.. code-block:: c

   /* Runs in the I2C target interrupt: short, no waiting */
   static int add(const struct twixt_req *req, uint8_t *rsp_data)
   {
   	if (req->len != 8) {
   		return -EINVAL;
   	}
   	sys_put_le32(sys_get_le32(&req->data[0]) + sys_get_le32(&req->data[4]), rsp_data);
   	return 4;
   }

   TWIXT_SERVICE_DEFINE(add_service, TWIXT_SERVICE_APP, add);

   twixt_request(peer, TWIXT_SERVICE_APP, req, 8, rsp, 4);

Native
------

Twister only builds samples; ``twixt_run.sh`` runs one process per MCU on a shared bus.

.. code-block:: console

   west twister -p native_sim -T modules/lib/twixt/samples/service
   modules/lib/twixt/scripts/twixt_run.sh modules/lib/twixt/samples/service pico nrf

.. code-block:: text

   [pico] <inf> service_sample: 0x11 adds 2 + 3 = 5
   [nrf] <inf> service_sample: 0x10 adds 40 + 2 = 42

Hardware
--------

Set up the boards as in the `main README <../../README.rst#on-real-boards>`_. Twister only builds samples, so flash them by hand.

.. code-block:: console

   west twister -p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T modules/lib/twixt/samples/service

   west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-service-pico modules/lib/twixt/samples/service
   west build -b nrf7002dk/nrf5340/cpuapp -d build-service-nrf modules/lib/twixt/samples/service
   west flash -d build-service-nrf
   smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if02 --timeout 60 upgrade --confirm build-service-pico/service/zephyr/zephyr.signed.bin

The Pico logs on ``-if00``, the DK on its VCOM.
