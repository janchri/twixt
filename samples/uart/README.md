# A UART over twixt

Three boards (a, b, c) and a gateway (gw). Each board's shell is on a `twixt,uart` to the gateway, which bridges it to a serial port for the laptop. Devicetree only: `board.overlay`, `gateway.overlay`.

```
 laptop ── pty-a ── uart-bridge ── bus-uart-a ══ I2C ══ bus-uart ── shell (board a)
           pty-b ── ...                                             (board b)
           pty-c ── ...                                             (board c)
```

## Build and run

```
twixt/scripts/twixt_run.sh twixt/samples/uart a b c gw
```

The gateway prints the PTY of each board (`[gw] pty-a connected to pseudotty: /dev/pts/7`). Open one with `tio` and press Enter:

```
mcu-a@10:~$ twixt ping 0x11
0x11: protocol 1, messages up to 64 bytes
```

Twister: `west twister -p native_sim -T twixt/samples/uart`

## On real boards

The gateway on a Raspberry Pi Pico, board b on an nRF7002 DK (`boards/`). The Pico runs at 3.3 V, the DK at 1.8 V: a level shifter between them, such as a PCA9306 breakout (with pull-ups on both sides):

| Signal | Pico | PCA9306 | nRF7002 DK |
|---|---|---|---|
| SDA | GP4 (pin 6) | SDA2 / SDA1 | P1.02 (Arduino SDA) |
| SCL | GP5 (pin 7) | SCL2 / SCL1 | P1.03 (Arduino SCL) |
| Reference | 3V3(OUT) (pin 36) | VREF2 / VREF1 | VDD (1.8 V) |
| Ground | GND (pin 38) | GND | GND |

No other MCU on the bus may answer at 0x10, 0x11, 0x12 or 0x70.

```
west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-uart-gw twixt/samples/uart -- -DMCU=gw
west build -b nrf7002dk/nrf5340/cpuapp -d build-uart-b twixt/samples/uart -- -DMCU=b
west flash -d build-uart-b
```

The Pico goes on as below. Its USB CDC-ACM ports, `/dev/serial/by-id/usb-twixt_twixt_pico-ifNN`: `-if00` the log, `-if02` MCUmgr, `-if04` the gateway's own shell, `-if06`, `-if08`, `-if0a` the boards a, b, c. Open board b's port with `tio` and press Enter (`twixt ping 0x70`); its log is on the DK's VCOM (`/dev/serial/by-id/usb-SEGGER_J-Link_*-if02`), 115200. Boards a and c on more DKs: `-DMCU=a` or `-DMCU=c`.

### The Pico under MCUboot

The Pico runs under MCUboot (`rpi_pico/rp2040/mcuboot`, `sysbuild.conf`), so that it updates itself over USB, e.g. in CI. The first time, MCUboot and the image go on together as a UF2: hold BOOTSEL while plugging the Pico in, and copy `build-uart-gw.uf2` to the RPI-RP2 drive:

```
python3 zephyr/scripts/build/mergehex.py --output-bin -o build-uart-gw.bin \
	build-uart-gw/mcuboot/zephyr/zephyr.hex build-uart-gw/uart/zephyr/zephyr.signed.hex
python3 zephyr/scripts/build/uf2conv.py -c -f 0xe48bff56 -b 0x10000000 -o build-uart-gw.uf2 build-uart-gw.bin
```

After that, every new image over MCUmgr, on the Pico's MCUmgr port (`-if02`):

```
smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if02 --timeout 60 upgrade --confirm build-uart-gw/uart/zephyr/zephyr.signed.bin
```

`--confirm` keeps the new image; without it MCUboot goes back to the old one at the next reset, unless the image is confirmed (`smpmgr image state-write --confirm`) once it runs.

### As a test, with twister

With `--device-testing`, twister flashes both boards: b on the DK is the DUT, the gateway on the Pico a required device (`sample.yaml`), and `pytest/test_uart.py` uses b's shell on the gateway's port `-if08`. The Pico is flashed over MCUmgr with twixt's `smp` runner (`scripts/runners/smp.py`), so MCUboot must be on it already (above). The hardware map: `../../scripts/twixt-hw.example.yaml`, with your DK's J-Link serial number and VCOM.

```
west twister --device-testing --hardware-map twixt-hw.yaml \
	-p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T twixt/samples/uart
```
