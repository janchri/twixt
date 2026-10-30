# Ping over twixt

`TWIXT_SERVICE_PING`: two MCUs, a (0x10) and b (0x11). Each pings the other until it answers, and logs its protocol version and longest message. Every MCU answers ping (`CONFIG_TWIXT_PING`, on by default); `twixt_ping()` fails with `-EPROTO` if the other MCU speaks another protocol or has other limits.

```c
struct twixt_ping_info info;

twixt_ping_wait(peer, &info, K_SECONDS(5));  /* while the other MCU boots */
twixt_ping(peer, &info);                     /* once */
```

## Build and run

```
twixt/scripts/twixt_run.sh twixt/samples/ping a b
```

```
[a] <inf> ping_sample: 0x11 answers: protocol 1, messages up to 64 bytes
[b] <inf> ping_sample: 0x10 answers: protocol 1, messages up to 64 bytes
```

Twister: `west twister -p native_sim -T twixt/samples/ping`

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
west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-ping-a twixt/samples/ping -- -DMCU=a
west build -b nrf7002dk/nrf5340/cpuapp -d build-ping-b twixt/samples/ping -- -DMCU=b
west flash -d build-ping-b
```

The Pico goes on as below. It logs on `/dev/serial/by-id/usb-twixt_twixt_pico-if00`, the DK on its VCOM (`/dev/serial/by-id/usb-SEGGER_J-Link_<serial number>-if02`), both 115200; the MCUs repeat every 2 s.

### The Pico under MCUboot

The Pico runs under MCUboot (`rpi_pico/rp2040/mcuboot`, `sysbuild.conf`), so that it updates itself over USB, e.g. in CI. The first time, MCUboot and the image go on together as a UF2: hold BOOTSEL while plugging the Pico in, and copy `build-ping-a.uf2` to the RPI-RP2 drive:

```
python3 zephyr/scripts/build/mergehex.py --output-bin -o build-ping-a.bin \
	build-ping-a/mcuboot/zephyr/zephyr.hex build-ping-a/ping/zephyr/zephyr.signed.hex
python3 zephyr/scripts/build/uf2conv.py -c -f 0xe48bff56 -b 0x10000000 -o build-ping-a.uf2 build-ping-a.bin
```

After that, every new image over MCUmgr, on the Pico's MCUmgr port, or with `west flash -r smp` as twister does:

```
smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if02 --timeout 60 upgrade --confirm build-ping-a/ping/zephyr/zephyr.signed.bin
```

`--confirm` keeps the new image; without it MCUboot goes back to the old one at the next reset, unless the image is confirmed (`smpmgr image state-write --confirm`) once it runs.

### As a test, with twister

With `--device-testing`, twister flashes both boards and `pytest/test_ping.py` reads both logs: b on the DK is the DUT, a on the Pico a required device (`sample.yaml`). The Pico is flashed over MCUmgr with twixt's `smp` runner (`scripts/runners/smp.py`), so MCUboot must be on it already (above). The hardware map: `../../scripts/twixt-hw.example.yaml`, with your DK's J-Link serial number and VCOM.

```
west twister --device-testing --hardware-map twixt-hw.yaml \
	-p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T twixt/samples/ping
```

Both platforms are needed: twister builds the Pico's image only for a platform of the run. `flash_before` opens the Pico's log port only after the update, as the Pico enumerates again.
