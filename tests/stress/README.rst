Stress
======

The echo load of every combination of senders against every combination of targets, and each board's shell through the bridge. Any wrong answer, error or silent shell fails it. The matrix shows ``ok/wrong/err``, rows send:

.. code-block:: text

   senders \ targets     nrf      pico  nrf+pico
   nrf                 0/0/0  1488/0/0  1488/0/0
   pico             1527/0/0     0/0/0  1527/0/0
   nrf+pico         1527/0/0  1489/0/0  2052/0/1

Native
------

.. code-block:: console

   west twister -p native_sim -T modules/lib/twixt/tests/stress

``--pytest-args=--seconds=5`` for longer cells. Native runs three MCUs, ``nrf2``, ``nrf`` and ``pico``.

Hardware
--------

Set up the boards as in the `main README <../../README.rst#on-real-boards>`_.

.. code-block:: console

   west twister --device-testing --hardware-map twixt-hw.yaml \
         -p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T modules/lib/twixt/tests/stress

By hand, on boards flashed by hand:

.. code-block:: console

   PICO=/dev/serial/by-id/usb-twixt_twixt_pico-
   pytest modules/lib/twixt/tests/stress/pytest --ports nrf=${PICO}if08,pico=${PICO}if04 --seconds 20

Options
-------

- ``--seconds``: per cell, default 1
- ``--interval``: ms between a sender's requests, default 5
- ``--ports``: real boards, ``<mcu>=<port>,...``
- ``--senders``: only these rows, e.g. ``nrf+pico``
- ``-s``: also print the calls of each sender

Classic ESP32
-------------

The ESP32 runs the same board application, Twixt services, shell and stress
commands as nRF, at address ``0x12``. Both boards' shells run over Twixt to the
Pico bridge. The ESP32 shell is the Pico's additional USB port, ``-if0a``;
``-if08`` remains nRF and ``-if04`` remains Pico. CP2102 is used for ESP32 flashing
and boot diagnostics.

Build the Pico with its optional ESP32 peer enabled; the default bridge retains
its original ports and peer list.

.. code-block:: console

   west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-stress-pico-esp modules/lib/twixt/tests/stress -- -DMCU=pico -Dstress_TWIXT_STRESS_ESP_PEER=ON
   west flash -d build-stress-pico-esp --domain stress -r smp --port /dev/serial/by-id/usb-twixt_twixt_pico-if02
   west build -b esp32_devkitc/esp32/procpu -d build-stress-esp modules/lib/twixt/tests/stress -- -DMCU=esp
   west flash -d build-stress-esp --esp-device /dev/ttyUSB0
   pytest modules/lib/twixt/tests/stress/pytest --ports nrf=/dev/serial/by-id/usb-twixt_twixt_pico-if08,pico=/dev/serial/by-id/usb-twixt_twixt_pico-if04,esp=/dev/serial/by-id/usb-twixt_twixt_pico-if0a --seconds 5

Three boards have seven non-empty subsets, so the matrix has 7 x 7 = 49 cells.
Self-transfers are skipped. See ``drivers/i2c/esp32/README.rst`` for the backend's
configuration and timing limits. The existing native test still uses its
original three simulated MCUs, nrf2, nrf and pico.
