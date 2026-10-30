# TWIxt, Two Wire Interface eXTended - A multi-master I2C bus

Requests and responses between the MCUs of one device, over the I2C bus they already share.

**No central MCU, no extra wires.** Every MCU is a target at its own address and a controller when it sends; twixt carries a request and its response in one I2C transfer, retries after a lost arbitration, and gives the requests a meaning through services: ping, settings, a UART for shells, MCUmgr, and your own.

*Devicetree and Kconfig only, on any Zephyr I2C driver that can be controller and target. A shared-memory bus runs the whole system on native_sim.*

> *Twixt* is old English for "between", as in "betwixt". Its "twi" is also TWI, the two-wire interface, as I2C is often called.

---

## Installation

Add twixt to your west manifest:

```yaml
manifest:
  remotes:
    - name: janchri
      url-base: https://github.com/janchri
  projects:
    - name: twixt
      remote: janchri
      revision: main
      path: modules/lib/twixt
```

Then fetch it, with the Python packages of its test harnesses:

```
west update twixt
west packages pip --install
```

<details>
<summary><strong>Standalone workspace</strong></summary>
<br>

twixt brings its own manifest, with Zephyr and the modules its samples need:

```
west init -m https://github.com/janchri/twixt twixt-workspace
cd twixt-workspace
west update
west packages pip --install
```

</details>

## How to Use

List the MCUs as children of the bus node, the same in every image, and say with `/chosen twixt,self` which one the image is. That enables `CONFIG_TWIXT`:

```
&i2c0 {
	mcu_a: mcu-a@10 { compatible = "twixt,mcu"; reg = <0x10>; };
	mcu_b: mcu-b@11 { compatible = "twixt,mcu"; reg = <0x11>; };
};

/ {
	chosen {
		twixt,self = &mcu_a;
	};
};
```

Ping the other MCU, or call a service of your own (ids from `TWIXT_SERVICE_APP`, 0x80, up), here one that adds two numbers (`samples/app`):

```c
#include <twixt/twixt.h>
#include <twixt/ping.h>

/* Runs in the I2C target interrupt: short, no waiting */
static int add(const struct twixt_req *req, uint8_t *rsp_data)
{
	if (req->len != 8) {
		return -EINVAL;
	}
	sys_put_le32(sys_get_le32(&req->data[0]) + sys_get_le32(&req->data[4]), rsp_data);
	return 4;
}

TWIXT_SERVICE_DEFINE(add_service, TWIXT_SERVICE_APP, add);

twixt_ping_wait(0x11, &info, K_SECONDS(5));
twixt_request(0x11, TWIXT_SERVICE_APP, req, 8, rsp, 4);
```

Run a sample on native_sim, one process per MCU on a shared bus:

```
twixt/scripts/twixt_run.sh twixt/samples/ping a b
```

## Services

| Service | Code | Interface |
|---|---|---|
| Ping (0x00) | `subsys/twixt/ping.c` | `<twixt/ping.h>`, `CONFIG_TWIXT_PING` |
| Echo (0x01) | `subsys/twixt/echo.c` | `<twixt/echo.h>`, `CONFIG_TWIXT_ECHO` |
| Settings (0x02) | `subsys/twixt/settings.c` | `<twixt/settings.h>`, `CONFIG_TWIXT_SETTINGS` |
| UART (0x10) | `drivers/serial/uart_twixt.c` | `dts/bindings/serial/twixt,uart.yaml` |
| MCUmgr (0x20) | `subsys/twixt/smp.c` | `<twixt/smp.h>`, `CONFIG_TWIXT_SMP` |

The ids are in `<twixt/twixt.h>`, the same on every MCU of the bus. With `CONFIG_TWIXT_SMP`, the MCU's MCUmgr answers SMP packets from the other MCUs, and `twixt_smp_send()` sends them packets: an MCU with Bluetooth or USB can pass MCUmgr on to the others, for example to update their images.

With `CONFIG_TWIXT_SHELL`, ping, echo and settings add their commands to the `twixt` shell command. `CONFIG_TWIXT_STRESS` adds an echo load on the bus that checks every answer (`subsys/twixt/stress.c`, `<twixt/stress.h>`), in the shell as `twixt stress`.

## Samples and tests

| Where | What |
|---|---|
| `samples/ping` | `TWIXT_SERVICE_PING`: does the other MCU answer, with the same protocol |
| `samples/echo` | `TWIXT_SERVICE_ECHO`: a text there and back |
| `samples/settings` | `TWIXT_SERVICE_SETTINGS`: get and set the other MCU's settings |
| `samples/uart` | `TWIXT_SERVICE_UART`: shells of three boards through a gateway; the gateway on a Raspberry Pi Pico |
| `samples/smp` | `TWIXT_SERVICE_SMP`: an MCUmgr request to the other MCU |
| `samples/app` | `TWIXT_SERVICE_APP`: a service of your own, adding two numbers |
| `tests/api` | One MCU: services, faults, the MCUmgr service's fragments |
| `tests/stress` | Echo load (`twixt stress`) of every combination of senders against every combination of targets: a gateway and two boards, on native_sim or real boards |

Twister runs every MCU of a sample, through its pytest harness:

```
west twister -p native_sim -T twixt
```

## On real boards

Every sample and `tests/stress` runs on two real boards as well: a Raspberry Pi Pico (`rpi_pico/rp2040/mcuboot`) and an nRF7002 DK (`nrf7002dk/nrf5340/cpuapp`), board files in each `boards/`. On both, the I2C block is only the target and the controller is software on the same pins (`twixt,i2c-mm-gpio`, see "On hardware" below).

**Wiring.** The Pico runs at 3.3 V, the DK at 1.8 V: a level shifter between them, such as a PCA9306 breakout, with pull-ups on both sides.

| Signal | Pico | PCA9306 | nRF7002 DK |
|---|---|---|---|
| SDA | GP4 (pin 6) | SDA2 / SDA1 | P1.02 (Arduino SDA) |
| SCL | GP5 (pin 7) | SCL2 / SCL1 | P1.03 (Arduino SCL) |
| Reference | 3V3(OUT) (pin 36) | VREF2 / VREF1 | VDD (1.8 V) |
| Ground | GND (pin 38) | GND | GND |

No other MCU on the bus may answer at the addresses of a test: 0x10, 0x11, and for `uart` and `stress` 0x12 and 0x70.

**The Pico under MCUboot.** The Pico has no debug probe, so it updates itself: its images run under MCUboot and take new ones over MCUmgr on USB. Once, MCUboot and a first image go on as a UF2 in BOOTSEL mode, e.g. from a sample (its README, "The Pico under MCUboot"):

```
west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-pico twixt/samples/ping -- -DMCU=a
python3 zephyr/scripts/build/mergehex.py --output-bin -o build-pico.bin \
	build-pico/mcuboot/zephyr/zephyr.hex build-pico/ping/zephyr/zephyr.signed.hex
python3 zephyr/scripts/build/uf2conv.py -c -f 0xe48bff56 -b 0x10000000 -o build-pico.uf2 build-pico.bin
```

Hold BOOTSEL while plugging the Pico in, and copy `build-pico.uf2` to the RPI-RP2 drive. From then on every twixt image is the USB device "twixt pico", with the same ports:

| Port (`/dev/serial/by-id/usb-twixt_twixt_pico-…`) | |
|---|---|
| `-if00` | log |
| `-if02` | MCUmgr: `west flash -r smp`, or `smpmgr --port … upgrade --confirm zephyr.signed.bin` |
| `-if04` | a gateway's own shell |
| `-if06`, `-if08`, `-if0a` | a gateway's bridges to the shells of boards a, b, c |

`west flash -r smp` is twixt's runner (`scripts/runners/smp.py`) for any image under MCUboot with MCUmgr: it sends the signed image, confirms it, and waits until the board is back on USB. It needs `smpmgr`, which `west packages pip --install` installs with twixt's other Python packages.

**Tests with twister.** In each hardware test b on the DK is the DUT and the Pico a required device, board a or the gateway (`harness_config: required_devices`). Twister flashes the DK with `nrfutil` and the Pico with the `smp` runner, and the pytest of the test reads both. A hardware map names the boards: copy `scripts/twixt-hw.example.yaml` and change the DK's J-Link serial number and VCOM.

```
west twister --device-testing --hardware-map twixt-hw.yaml \
	-p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T twixt
```

Both platforms are needed, as twister builds the Pico's image only for a platform of the run. Without `--device-testing`, twister only builds the images for the two boards.

On WSL, `usbipd` has to give the Pico to WSL again after every update, as it enumerates anew: `usbipd attach --wsl --busid <busid> --auto-attach` on the Windows side keeps doing that.

---

## Advanced

<details>
<summary><strong>How it works</strong></summary>
<br>

```
 services    ping   echo   settings   UART   MCUmgr   (yours, 0x80..)
                    \     \      |       /      /
 twixt       request = service, src, flags, data;  response = status, data
                               |
 link         one write + read transfer per message, retries
                               |
 I2C driver   any Zephyr I2C driver; i2c_mm_native on native_sim
```

Each layer only knows the one below. Details are in the headers and bindings:

| Layer | Code | Interface |
|---|---|---|
| Link | `drivers/i2c/i2c_mm.c` | `<drivers/i2c/i2c_mm.h>`, `CONFIG_I2C_MM` |
| twixt | `subsys/twixt/twixt.c` | `<twixt/twixt.h>`, `CONFIG_TWIXT` |
| Bus on native_sim | `drivers/i2c/i2c_mm_native.c` | `dts/bindings/i2c/twixt,native-i2c-mm.yaml`, `<drivers/i2c/i2c_mm_native.h>` |
| Bus from a target block and GPIOs | `drivers/i2c/i2c_mm_gpio.c` | `dts/bindings/i2c/twixt,i2c-mm-gpio.yaml` |

</details>

<details>
<summary><strong>On hardware</strong></summary>
<br>

The I2C driver must be controller and target on one block, report a lost arbitration as `-EAGAIN`, recover a stuck bus before its START, report STOP to the target, and let the target answer within a byte time (or stretch SCL).

Where it cannot, `twixt,i2c-mm-gpio` keeps the I2C block as the target and clocks the MCU's own transfers in software on the same pins: the nRF TWIS (the TWIM does not detect lost arbitration) and the RP2040 (`snps,designware-i2c` is not controller and target at once). Good candidates for the product MCUs: STM32 I2C v2, NXP LPI2C.

</details>

<details>
<summary><strong>Debugging on native_sim</strong></summary>
<br>

With `GDB="<mcu>..."`, `scripts/twixt_run.sh` builds these MCUs for debugging and runs them under `gdbserver :2345`, `:2346` and so on:

```
GDB="b" twixt/scripts/twixt_run.sh twixt/samples/ping a b
gdb /tmp/twixt-gdb-2345.exe -ex 'target remote :2345'
```

</details>

<details>
<summary><strong>Coding style</strong></summary>
<br>

C follows Zephyr's `.clang-format`, Python Zephyr's ruff rules (`.ruff.toml`). A local pre-commit hook runs the tools of `requirements.txt` from the active venv:

```
pip install -r twixt/requirements.txt
cd twixt && pre-commit install
```

</details>

## License

Apache-2.0, see [LICENSE](LICENSE).
