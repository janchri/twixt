Ping
====

Two MCUs, ``pico`` (0x10) and ``nrf`` (0x11), ping each other and log the protocol version and the longest message.

.. code-block:: c

   struct twixt_ping_info info;

   twixt_ping_wait(peer, &info, K_SECONDS(5));  /* while the other MCU boots */
   twixt_ping(peer, &info);                     /* once */

Native
------

Twister only builds samples; ``twixt_run.sh`` runs one process per MCU on a shared bus.

.. code-block:: console

   west twister -p native_sim -T modules/lib/twixt/samples/ping
   modules/lib/twixt/scripts/twixt_run.sh modules/lib/twixt/samples/ping pico nrf

.. code-block:: text

   [pico] <inf> ping_sample: 0x11 answers: protocol 1, messages up to 64 bytes
   [nrf] <inf> ping_sample: 0x10 answers: protocol 1, messages up to 64 bytes

Hardware
--------

Set up the boards as in the `main README <../../README.rst#on-real-boards>`_. Twister only builds samples, so flash them by hand.

.. code-block:: console

   west twister -p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T modules/lib/twixt/samples/ping

   west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-ping-pico modules/lib/twixt/samples/ping
   west build -b nrf7002dk/nrf5340/cpuapp -d build-ping-nrf modules/lib/twixt/samples/ping
   west flash -d build-ping-nrf
   smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if02 --timeout 60 upgrade --confirm build-ping-pico/ping/zephyr/zephyr.signed.bin

The Pico logs on ``-if00``, the DK on its VCOM.
