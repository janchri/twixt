MCUmgr through a serial gateway
===============================

Stock SMP tools update every MCU, without a shell: the Pico gives each MCU a USB port (``twixt,smp-gateway-serial``) and passes the packets to that MCU's MCUmgr over the bus. Both MCUs run under MCUboot.

.. code-block:: text

   host: smpmgr ── USB port per MCU ── pico (0x10) ══ I2C ══ nrf (0x11)

``-if04`` reaches nrf, ``-if06`` reaches pico. One request at a time over all ports.

Hardware
--------

Real boards only. Set up the boards as in the `main README <../../../README.rst#on-real-boards>`_. Twister only builds samples, so flash them by hand.

.. code-block:: console

   west twister -p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T modules/lib/twixt/samples/smp/serial

   west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-serial-pico modules/lib/twixt/samples/smp/serial
   west build --sysbuild -b nrf7002dk/nrf5340/cpuapp -d build-serial-nrf modules/lib/twixt/samples/smp/serial
   west flash -d build-serial-nrf
   smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if02 --timeout 60 upgrade --confirm build-serial-pico/serial/zephyr/zephyr.signed.bin

Then update either MCU:

.. code-block:: console

   smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if04 image state-read
   smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if04 --timeout 60 upgrade --confirm zephyr.signed.bin
