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
