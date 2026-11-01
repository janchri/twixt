Image update
============

The real ``smpmgr`` updates nrf through the bus, back and forth, and restarts an upload it killed halfway. That a version runs means MCUboot accepted its signature and hash.

.. code-block:: text

   host: smpmgr ── USB, serial SMP ── pico ══ I2C ══ nrf: MCUmgr, MCUboot

Hardware
--------

Set up the boards as in the `main README <../../README.rst#on-real-boards>`_. Twister flashes both boards and builds nrf twice, with two versions. Without ``--device-testing`` it only builds.

.. code-block:: console

   west twister --device-testing --hardware-map twixt-hw.yaml \
         -p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T modules/lib/twixt/tests/smp_update

By hand, on boards flashed by hand:

.. code-block:: console

   pytest modules/lib/twixt/tests/smp_update/pytest --port /dev/serial/by-id/usb-twixt_twixt_pico-if04 \
       --v1 v1.signed.bin --v2 v2.signed.bin

Over Bluetooth
--------------

By hand. nrf advertises one device per MCU, and ``hci_ipc`` runs the controller on its network core.

.. code-block:: console

   west build -p always --sysbuild -b nrf7002dk/nrf5340/cpuapp -d build-ble modules/lib/twixt/tests/smp_update -- \
       -DMCU=nrf -DROUTE=ble -DVERSION=1 -DSB_CONFIG_NET_CORE_IMAGE_HCI_IPC=y
   west flash -d build-ble

   pytest modules/lib/twixt/tests/smp_update/pytest --ble nrf --ble-other pico --v1 v1.signed.bin --v2 v2.signed.bin

``--v1`` and ``--v2`` are images of the MCU behind ``--ble``.
