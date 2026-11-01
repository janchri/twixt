Multi MCU
=========

Two MCUs use each service on the other: ping, echo, a service of the test, settings and MCUmgr. Settings must survive a restart. A request runs once or not at all, over 100 rounds of raw transfers: on the nRF, only real boards prove the STOP detection of ``twixt,i2c-mm-gpio``.

Native
------

.. code-block:: console

   west twister -p native_sim -T modules/lib/twixt/tests/multi_mcu

Hardware
--------

Set up the boards as in the `main README <../../README.rst#on-real-boards>`_.

.. code-block:: console

   west twister --device-testing --hardware-map twixt-hw.yaml \
         -p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T modules/lib/twixt/tests/multi_mcu
