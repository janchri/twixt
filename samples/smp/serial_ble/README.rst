MCUmgr through a serial and a Bluetooth gateway
===============================================

As ``../serial``, and the nRF advertises one Bluetooth device per MCU (``twixt,smp-gateway-bt``), named after the MCU.

.. code-block:: text

   host: smpmgr ── USB port per MCU ── pico (0x10) ══ I2C ══ nrf (0x11) ── BLE device per MCU ── smpmgr

Over USB, ``-if04`` reaches nrf and ``-if06`` reaches pico. Over Bluetooth, the devices are ``nrf`` and ``pico``.

Bluetooth asks for no pairing yet: anyone in range can talk SMP to every MCU, and only MCUboot's signature check protects the images.

Hardware
--------

Real boards only. Set up the boards as in the `main README <../../../README.rst#on-real-boards>`_. Twister only builds samples, so flash them by hand.

.. code-block:: console

   west twister -p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T modules/lib/twixt/samples/smp/serial_ble

   west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-serial_ble-pico modules/lib/twixt/samples/smp/serial_ble
   west build --sysbuild -b nrf7002dk/nrf5340/cpuapp -d build-serial_ble-nrf modules/lib/twixt/samples/smp/serial_ble
   west flash -d build-serial_ble-nrf
   smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if02 --timeout 60 upgrade --confirm build-serial_ble-pico/serial_ble/zephyr/zephyr.signed.bin

Then update either MCU:

.. code-block:: console

   smpmgr --ble pico image state-read
   smpmgr --ble pico --timeout 60 upgrade --confirm zephyr.signed.bin

A device does not advertise while connected. The gateway drops a connection after ``CONFIG_TWIXT_SMP_GATEWAY_BT_IDLE_MS`` without SMP traffic.
