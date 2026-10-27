# TWIxt, Two Wire Interface eXTended - A multi-master I2C bus

Requests and responses between the MCUs of one device, over the I2C bus they already share.

**No central MCU, no extra wires.** Every MCU is a target at its own address and a controller when it sends; twixt carries a request and its response in one I2C transfer, retries after a lost arbitration, and gives the requests a meaning through services: ping, settings, a UART for shells and MCUmgr, and your own.

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

Ping the other MCU, or call a service of your own (ids from `TWIXT_SERVICE_APP`, 0x80, up):

```c
#include <twixt/twixt.h>
#include <twixt/ping.h>

/* Runs in the I2C target interrupt: short, and only copying */
static int echo(const struct twixt_req *req, uint8_t *rsp_data)
{
	memcpy(rsp_data, req->data, req->len);
	return req->len;
}

TWIXT_SERVICE_DEFINE(echo_service, TWIXT_SERVICE_APP, echo);

twixt_ping_wait(0x11, &info, K_SECONDS(5));
twixt_request(0x11, TWIXT_SERVICE_APP, "hi", 2, rsp, 2);
```

Run a sample on native_sim, one process per MCU on a shared bus:

```
twixt/scripts/twixt_run.sh twixt/samples/hello a b
```

## Services

| Service | Code | Interface |
|---|---|---|
| Ping (0x00) | `subsys/twixt/ping.c` | `<twixt/ping.h>`, `CONFIG_TWIXT_PING` |
| Settings (0x01) | `subsys/twixt/settings.c` | `<twixt/settings.h>`, `CONFIG_TWIXT_SETTINGS` |
| UART (0x10) | `drivers/serial/uart_twixt.c` | `dts/bindings/serial/twixt,uart.yaml` |

With `CONFIG_TWIXT_SHELL`, ping and settings add their commands to the `twixt` shell command.

## Samples and tests

| Where | What |
|---|---|
| `samples/hello` | Ping, and a service of the application's own |
| `samples/settings` | Get and set the other MCU's settings |
| `samples/uart` | Shells of three boards through a gateway; the gateway on a Raspberry Pi Pico |
| `samples/smp` | MCUmgr to a board through the gateway |
| `tests/api` | One MCU: services, faults |
| `tests/stress` | Three MCUs sending at the same moments |

Twister runs every MCU of a sample, through its pytest harness:

```
west twister -p native_sim -T twixt
```

---

## Advanced

<details>
<summary><strong>How it works</strong></summary>
<br>

```
 services    ping          settings          UART            (yours, 0x80..)
                  \            |              /
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

</details>

<details>
<summary><strong>On hardware</strong></summary>
<br>

The I2C driver must be controller and target on one block, report a lost arbitration as `-EAGAIN`, recover a stuck bus before its START, report STOP to the target, and let the target answer within a byte time (or stretch SCL).

The RP2040 (`snps,designware-i2c`) is enough for a console gateway: it needs `CONFIG_I2C_MM_TARGET_REREGISTER`, reports lost arbitration as `-EIO`, and is no target during its own transfers. Good candidates for the product MCUs: STM32 I2C v2, NXP LPI2C.

</details>

<details>
<summary><strong>Debugging on native_sim</strong></summary>
<br>

With `GDB="<mcu>..."`, `scripts/twixt_run.sh` builds these MCUs for debugging and runs them under `gdbserver :2345`, `:2346` and so on:

```
GDB="b" twixt/scripts/twixt_run.sh twixt/samples/hello a b
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
