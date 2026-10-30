# MCUmgr over twixt

`TWIXT_SERVICE_SMP`: two MCUs, a (0x10) and b (0x11), with MCUmgr on the twixt MCUmgr service (`CONFIG_TWIXT_SMP`, `<twixt/smp.h>`). b sends a an os echo request; a's MCUmgr answers. SMP packets go over the bus as they are, cut into requests, with a CRC: no shell, UART or base64 in between.

```c
twixt_smp_send(0x10, pkt, len);                         /* an SMP request packet */
struct net_buf *rsp = twixt_smp_rsp_get(&src, K_SECONDS(5));  /* its response */
```

An MCU with Bluetooth or USB passes the packets of a laptop or phone on the same way, one MCU per connection, e.g. to update every MCU of the bus. MCUmgr on a shell over the bus works too: see `../uart`, with `CONFIG_MCUMGR_TRANSPORT_SHELL` on the board.

## Build and run

```
twixt/scripts/twixt_run.sh twixt/samples/smp a b
```

```
[a] <inf> smp_sample: mcu-a@10 at 0x10: MCUmgr on the twixt service
[b] <inf> smp_sample: mcu-b@11 at 0x11: MCUmgr on the twixt service
[b] <inf> smp_sample: MCUmgr echo from 0x10: hello over the bus
```

Twister: `west twister -p native_sim -T twixt/samples/smp`

## On real boards

a on a Raspberry Pi Pico, b on an nRF7002 DK (`boards/`). The Pico runs at 3.3 V, the DK at 1.8 V: a level shifter between them, such as a PCA9306 breakout (with pull-ups on both sides):

| Signal | Pico | PCA9306 | nRF7002 DK |
|---|---|---|---|
| SDA | GP4 (pin 6) | SDA2 / SDA1 | P1.02 (Arduino SDA) |
| SCL | GP5 (pin 7) | SCL2 / SCL1 | P1.03 (Arduino SCL) |
| Reference | 3V3(OUT) (pin 36) | VREF2 / VREF1 | VDD (1.8 V) |
| Ground | GND (pin 38) | GND | GND |

No other MCU on the bus may answer at 0x10 or 0x11.

```
west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-smp-a twixt/samples/smp -- -DMCU=a
west build -b nrf7002dk/nrf5340/cpuapp -d build-smp-b twixt/samples/smp -- -DMCU=b
west flash -d build-smp-b
```

The Pico goes on as below. It logs on `/dev/serial/by-id/usb-twixt_twixt_pico-if00`, the DK on its VCOM (`/dev/serial/by-id/usb-SEGGER_J-Link_<serial number>-if02`), both 115200; the MCUs repeat every 2 s.

### The Pico under MCUboot

The Pico runs under MCUboot (`rpi_pico/rp2040/mcuboot`, `sysbuild.conf`), so that it updates itself over USB, e.g. in CI. The first time, MCUboot and the image go on together as a UF2: hold BOOTSEL while plugging the Pico in, and copy `build-smp-a.uf2` to the RPI-RP2 drive:

```
python3 zephyr/scripts/build/mergehex.py --output-bin -o build-smp-a.bin \
	build-smp-a/mcuboot/zephyr/zephyr.hex build-smp-a/smp/zephyr/zephyr.signed.hex
python3 zephyr/scripts/build/uf2conv.py -c -f 0xe48bff56 -b 0x10000000 -o build-smp-a.uf2 build-smp-a.bin
```

After that, every new image over MCUmgr, on the Pico's MCUmgr port, or with `west flash -r smp` as twister does:

```
smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if02 --timeout 60 upgrade --confirm build-smp-a/smp/zephyr/zephyr.signed.bin
```

`--confirm` keeps the new image; without it MCUboot goes back to the old one at the next reset, unless the image is confirmed (`smpmgr image state-write --confirm`) once it runs.

### As a test, with twister

With `--device-testing`, twister flashes both boards and `pytest/test_smp.py` reads both logs: b on the DK is the DUT, a on the Pico a required device (`sample.yaml`). The Pico is flashed over MCUmgr with twixt's `smp` runner (`scripts/runners/smp.py`), so MCUboot must be on it already (above). The hardware map: `../../scripts/twixt-hw.example.yaml`, with your DK's J-Link serial number and VCOM.

```
west twister --device-testing --hardware-map twixt-hw.yaml \
	-p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T twixt/samples/smp
```

Both platforms are needed: twister builds the Pico's image only for a platform of the run. `flash_before` opens the Pico's log port only after the update, as the Pico enumerates again.
