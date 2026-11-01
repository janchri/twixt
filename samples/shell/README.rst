Shell over the bus
==================

Each board's shell runs on a ``twixt,uart`` to a bridge, ``pico``, which passes it on to a serial port. Devicetree only: ``board.overlay``, ``bridge.overlay``.

.. code-block:: text

   host ── port ── uart-bridge ── bus-uart-nrf ══ I2C ══ bus-uart ── shell of nrf

Native
------

Twister only builds samples; ``twixt_run.sh`` runs one process per MCU on a shared bus.

.. code-block:: console

   west twister -p native_sim -T modules/lib/twixt/samples/shell
   modules/lib/twixt/scripts/twixt_run.sh modules/lib/twixt/samples/shell nrf2 nrf nrf3 pico

The bridge prints the PTY of each board. Open one with ``tio`` and press Enter:

.. code-block:: text

   nrf2@10:~$ twixt ping 0x11
   0x11: protocol 1, messages up to 64 bytes

Hardware
--------

Set up the boards as in the `main README <../../README.rst#on-real-boards>`_. Twister only builds samples, so flash them by hand.

.. code-block:: console

   west twister -p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T modules/lib/twixt/samples/shell

   west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-shell-pico modules/lib/twixt/samples/shell
   west build -b nrf7002dk/nrf5340/cpuapp -d build-shell-nrf modules/lib/twixt/samples/shell -- -DMCU=nrf
   west flash -d build-shell-nrf
   smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if02 --timeout 60 upgrade --confirm build-shell-pico/shell/zephyr/zephyr.signed.bin

Open nrf's shell on ``-if08`` with ``tio`` and press Enter. More DKs join with ``-DMCU=nrf2`` or ``-DMCU=nrf3``. No other MCU on the bus may answer at 0x10, 0x11, 0x12 or 0x70.
