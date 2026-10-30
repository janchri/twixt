# Settings over twixt

Two MCUs, a (0x10) and b (0x11), get and set each other's Zephyr settings by name:

| Setting        | Access     | Value                             |
|----------------|------------|-----------------------------------|
| `mcu/name`     | read-only  | devicetree node, e.g. `mcu-a@10`  |
| `mcu/greeting` | read/write | text, kept in flash (NVS)         |

```c
twixt_settings_get(peer, "mcu/name", val, sizeof(val));
twixt_settings_set(peer, "mcu/greeting", hi, len);
twixt_settings_set(peer, "mcu/name", "x", 1);  /* -EACCES */
```

The handlers run in the I2C target interrupt, see `<twixt/settings.h>`.

## Build and run

```
twixt/scripts/twixt_run.sh twixt/samples/settings a b
```

```
[a] <inf> settings_sample: mcu-a@10 at 0x10, greeting from flash: ""
[a] <inf> settings_sample: 0x11 is mcu-b@11
[a] <inf> settings_sample: 0x11 mcu/greeting = "hi from mcu-a@10"
[a] <inf> settings_sample: 0x11 mcu/name is read-only: -13
```

Run it again with `SKIP_BUILD=1`: each MCU boots with the greeting the other gave it.

Twister, which also restarts b to check its flash: `west twister -p native_sim -T twixt/samples/settings`

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
west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-settings-a twixt/samples/settings -- -DMCU=a
west build -b nrf7002dk/nrf5340/cpuapp -d build-settings-b twixt/samples/settings -- -DMCU=b
west flash -d build-settings-b
```

The Pico goes on as below. It logs on `/dev/serial/by-id/usb-twixt_twixt_pico-if00`, the DK on its VCOM (`/dev/serial/by-id/usb-SEGGER_J-Link_<serial number>-if02`), both 115200; the MCUs repeat every 5 s.

### The Pico under MCUboot

The Pico runs under MCUboot (`rpi_pico/rp2040/mcuboot`, `sysbuild.conf`), so that it updates itself over USB, e.g. in CI. The first time, MCUboot and the image go on together as a UF2: hold BOOTSEL while plugging the Pico in, and copy `build-settings-a.uf2` to the RPI-RP2 drive:

```
python3 zephyr/scripts/build/mergehex.py --output-bin -o build-settings-a.bin \
	build-settings-a/mcuboot/zephyr/zephyr.hex build-settings-a/settings/zephyr/zephyr.signed.hex
python3 zephyr/scripts/build/uf2conv.py -c -f 0xe48bff56 -b 0x10000000 -o build-settings-a.uf2 build-settings-a.bin
```

After that, every new image over MCUmgr, on the Pico's MCUmgr port, or with `west flash -r smp` as twister does:

```
smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if02 --timeout 60 upgrade --confirm build-settings-a/settings/zephyr/zephyr.signed.bin
```

`--confirm` keeps the new image; without it MCUboot goes back to the old one at the next reset, unless the image is confirmed (`smpmgr image state-write --confirm`) once it runs.

### As a test, with twister

With `--device-testing`, twister flashes both boards and `pytest/test_settings.py` reads both logs: b on the DK is the DUT, a on the Pico a required device (`sample.yaml`). The Pico is flashed over MCUmgr with twixt's `smp` runner (`scripts/runners/smp.py`), so MCUboot must be on it already (above). The hardware map: `../../scripts/twixt-hw.example.yaml`, with your DK's J-Link serial number and VCOM.

```
west twister --device-testing --hardware-map twixt-hw.yaml \
	-p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T twixt/samples/settings
```

Both platforms are needed: twister builds the Pico's image only for a platform of the run. `flash_before` opens the Pico's log port only after the update, as the Pico enumerates again.
