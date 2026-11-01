Settings
========

Two MCUs get and set each other's Zephyr settings by name: ``mcu/name`` is read-only, ``mcu/greeting`` is kept in flash. The handlers run in the I2C target interrupt.

.. code-block:: c

   twixt_settings_get(peer, "mcu/name", val, sizeof(val));
   twixt_settings_set(peer, "mcu/greeting", hi, len);
   twixt_settings_set(peer, "mcu/name", "x", 1);  /* -EACCES */

Native
------

Twister only builds samples; ``twixt_run.sh`` runs one process per MCU on a shared bus.

.. code-block:: console

   west twister -p native_sim -T modules/lib/twixt/samples/settings
   modules/lib/twixt/scripts/twixt_run.sh modules/lib/twixt/samples/settings pico nrf

.. code-block:: text

   [pico] <inf> settings_sample: 0x11 is nrf@11
   [pico] <inf> settings_sample: 0x11 mcu/greeting = "hi from pico@10"
   [pico] <inf> settings_sample: 0x11 mcu/name is read-only: -13

Run it again with ``SKIP_BUILD=1``: each MCU boots with the greeting the other gave it.

Hardware
--------

Set up the boards as in the `main README <../../README.rst#on-real-boards>`_. Twister only builds samples, so flash them by hand.

.. code-block:: console

   west twister -p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T modules/lib/twixt/samples/settings

   west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-settings-pico modules/lib/twixt/samples/settings
   west build -b nrf7002dk/nrf5340/cpuapp -d build-settings-nrf modules/lib/twixt/samples/settings
   west flash -d build-settings-nrf
   smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if02 --timeout 60 upgrade --confirm build-settings-pico/settings/zephyr/zephyr.signed.bin

The Pico logs on ``-if00``, the DK on its VCOM.
