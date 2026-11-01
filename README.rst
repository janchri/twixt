TWIxt, Two Wire Interface eXTended
==================================

Requests and responses between the MCUs of one device, over the I2C bus they already share. Every MCU is a target at its own address and a controller when it sends; twixt carries a request and its response in one I2C transfer, retries after a lost arbitration, and gives requests a meaning through services.

Devicetree and Kconfig only, on any Zephyr I2C driver that can be controller and target. A shared-memory bus runs the whole system on ``native_sim``.

Installation
------------

Add twixt to your west manifest:

.. code-block:: yaml

   manifest:
     remotes:
       - name: janchri
         url-base: https://github.com/janchri
     projects:
       - name: twixt
         remote: janchri
         revision: main
         path: modules/lib/twixt

.. code-block:: console

   west update twixt
   west packages pip --install

Or as a standalone workspace, where the commands below use ``twixt`` instead of ``modules/lib/twixt``:

.. code-block:: console

   west init -m https://github.com/janchri/twixt twixt-workspace
   cd twixt-workspace
   west update
   west packages pip --install

Usage
-----

List the MCUs under the bus node, the same in every image, and say with ``/chosen twixt,self`` which one the image is. That enables ``CONFIG_TWIXT``.

.. code-block:: dts

   &i2c0 {
   	mcu_pico: pico@10 { compatible = "twixt,mcu"; reg = <0x10>; };
   	mcu_nrf: nrf@11 { compatible = "twixt,mcu"; reg = <0x11>; };
   };

   / {
   	chosen {
   		twixt,self = &mcu_pico;
   	};
   };

Then ping the other MCU, or call a service of your own, from ``TWIXT_SERVICE_APP`` up:

.. code-block:: c

   /* Runs in the I2C target interrupt: short, no waiting */
   static int add(const struct twixt_req *req, uint8_t *rsp_data)
   {
   	sys_put_le32(sys_get_le32(&req->data[0]) + sys_get_le32(&req->data[4]), rsp_data);
   	return 4;
   }

   TWIXT_SERVICE_DEFINE(add_service, TWIXT_SERVICE_APP, add);

   twixt_ping_wait(0x11, &info, K_SECONDS(5));
   twixt_request(0x11, TWIXT_SERVICE_APP, req, 8, rsp, 4);

Services
--------

- Ping, 0x00: ``<twixt/service/ping.h>``, ``CONFIG_TWIXT_PING``
- Echo, 0x01: ``<twixt/service/echo.h>``, ``CONFIG_TWIXT_ECHO``
- Settings, 0x02: ``<twixt/service/settings.h>``, ``CONFIG_TWIXT_SETTINGS``
- UART, 0x10: ``twixt,uart``, ``drivers/serial/uart_twixt.c``
- MCUmgr, 0x20: ``<twixt/service/smp.h>``, ``CONFIG_TWIXT_SMP``

``CONFIG_TWIXT_SHELL`` adds the ``twixt`` shell command. ``CONFIG_TWIXT_STRESS`` adds an echo load that checks every answer, in the shell as ``twixt stress``.

Updating every MCU
------------------

A gateway gives stock SMP tools such as ``smpmgr`` one endpoint per MCU, without a shell: a serial port with ``twixt,smp-gateway-serial``, a Bluetooth device with ``twixt,smp-gateway-bt``. List one node per MCU to reach, itself included. See ``samples/smp``.

Bluetooth asks for no pairing yet: anyone in range can talk SMP to every MCU, and only MCUboot's signature check protects the images.

Samples
-------

Images to flash and try; twister only builds them.

- ``samples/ping``: does the other MCU answer
- ``samples/service``: a service of your own
- ``samples/settings``: the other MCU's settings
- ``samples/shell``: the shell of an MCU with no USB, through a bridge
- ``samples/button_led``: a button on the DK toggles the LED on the Pico
- ``samples/smp/serial``: stock SMP tools reach every MCU over USB
- ``samples/smp/serial_ble``: the same, and over Bluetooth

Tests
-----

- ``tests/link``: the link and ``twixt,i2c-mm-gpio`` on a fake bus
- ``tests/services``: services and faults, one MCU
- ``tests/smp_gateway``: the core of the MCUmgr gateway
- ``tests/multi_mcu``: all services between two MCUs
- ``tests/stress``: the load of every sender against every target
- ``tests/smp_update``: real image updates with ``smpmgr``

.. code-block:: console

   west twister -p native_sim -T modules/lib/twixt
   west twister --device-testing --hardware-map twixt-hw.yaml \
         -p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T modules/lib/twixt

Both platforms are needed, as twister builds the Pico's image only for a platform of the run. A hardware map names the boards: copy ``scripts/twixt-hw.example.yaml``. Without ``--device-testing``, twister only builds for the boards.

On real boards
--------------

A Raspberry Pi Pico and an nRF7002 DK. On both, the I2C block is only the target and the controller is software on the same pins (``twixt,i2c-mm-gpio``).

The Pico runs at 3.3 V, the DK at 1.8 V: a level shifter between them, such as a PCA9306 breakout, with pull-ups on both sides.

- SDA: Pico GP4 (pin 6), PCA9306 SDA2 / SDA1, DK P1.02
- SCL: Pico GP5 (pin 7), PCA9306 SCL2 / SCL1, DK P1.03
- Reference: Pico 3V3(OUT) (pin 36), PCA9306 VREF2 / VREF1, DK VDD (1.8 V)
- Ground: Pico GND (pin 38), PCA9306 GND, DK GND

The Pico has no debug probe, so its images run under MCUboot and update over USB. Once, MCUboot and a first image go on as a UF2: hold BOOTSEL while plugging it in, and copy ``build-pico.uf2`` to the RPI-RP2 drive.

.. code-block:: console

   west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-pico modules/lib/twixt/samples/ping
   python3 zephyr/scripts/build/mergehex.py --output-bin -o build-pico.bin \
       build-pico/mcuboot/zephyr/zephyr.hex build-pico/ping/zephyr/zephyr.signed.hex
   python3 zephyr/scripts/build/uf2conv.py -c -f 0xe48bff56 -b 0x10000000 -o build-pico.uf2 build-pico.bin

From then on every twixt image is the USB device "twixt pico", ``/dev/serial/by-id/usb-twixt_twixt_pico-ifNN``:

- ``-if00``: log
- ``-if02``: MCUmgr, for ``west flash -r smp`` or ``smpmgr ... upgrade --confirm zephyr.signed.bin``
- ``-if04``: a bridge's own shell, or a gateway's port to nrf
- ``-if06``, ``-if08``, ``-if0a``: a bridge's ports to the boards at 0x10, 0x11, 0x12

``--confirm`` keeps the new image; without it, MCUboot goes back to the old one at the next reset. The DK logs on its VCOM, at 115200.

On WSL, ``usbipd attach --wsl --busid <busid> --auto-attach`` gives the Pico back to WSL after every update, as it enumerates anew.

Layers
------

- Services: ``subsys/twixt/service``, ``uart_twixt.c``, your own
- Clients of services: ``subsys/twixt/client``, the SMP gateway and the echo load
- twixt: ``subsys/twixt/twixt.c``, ``<twixt/twixt.h>``
- Link, one request and response per transfer: ``subsys/twixt/link.c``, ``<twixt/link.h>``
- I2C driver: any capable Zephyr driver, ``drivers/i2c/i2c_mm_gpio.c``, or ``drivers/i2c/native``

The driver must be controller and target on one block, report a lost arbitration as ``-EAGAIN``, recover a stuck bus before its START, report STOP to the target, and let the target answer within a byte time. Where it cannot, ``twixt,i2c-mm-gpio`` keeps the block as the target and clocks transfers in software: the nRF TWIS and the RP2040. Good candidates for hardware only: STM32 I2C v2, NXP LPI2C.

Debugging on native_sim
-----------------------

With ``GDB="<mcu>..."``, ``twixt_run.sh`` runs these MCUs under ``gdbserver :2345``, ``:2346`` and so on.

.. code-block:: console

   GDB="nrf" modules/lib/twixt/scripts/twixt_run.sh modules/lib/twixt/samples/ping pico nrf
   gdb /tmp/twixt-gdb-2345.exe -ex 'target remote :2345'

Coding style
------------

C follows Zephyr's ``.clang-format``, Python Zephyr's ruff rules. A local pre-commit hook runs the tools:

.. code-block:: console

   pip install -r modules/lib/twixt/requirements-dev.txt
   cd modules/lib/twixt && pre-commit install

License
-------

Apache-2.0, see ``LICENSE``.
