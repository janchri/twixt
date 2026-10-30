# Stress test

The echo load of every combination of senders against every combination of targets, on a gateway (gw, 0x70) and two boards (a, 0x10; b, 0x11). Each sender of a cell runs `twixt stress start <interval> <seconds> <targets>` in its shell, all at the same time; `twixt stress` then has the counts. A cell fails on a wrong answer or a shell that does not answer; errors (retries exhausted) are only counted. At the end the matrix, `ok/wrong/err` per cell, rows send, columns are the targets:

```
senders \ targets         a        b       gw      a+b     a+gw     b+gw   a+b+gw
a                     0/0/0   59/0/0   57/0/0   56/0/0   59/0/0   73/0/0   73/0/0
...
a+b+gw               92/0/0   94/0/0   90/0/0   94/0/2   96/0/2  106/0/2  124/0/2
```

The boards' shells are on a UART over the bus to the gateway (`board.overlay`), which bridges them to ports on the host and has its own shell on a third (`gateway.overlay`). The test (`pytest/`) talks to all three.

| Option | Default | |
|---|---|---|
| `--seconds` | 1 | per cell |
| `--interval` | 5 | ms between the echo requests of a sender |
| `--ports` | | real boards: `<mcu>=<port>,...`; the matrix has only these MCUs |
| `--senders` | | only these rows: `<combo>,...`, e.g. `a+b+gw` |

## On native_sim

Twister builds the three MCUs and starts the gateway; the test starts the boards and opens the gateway's PTYs:

```
west twister -p native_sim -T twixt/tests/stress
```

About 90 s; `--pytest-args=--seconds=5` for longer cells.

## On real boards

The gateway on a Raspberry Pi Pico, board b on an nRF7002 DK (`boards/`). The Pico runs at 3.3 V, the DK at 1.8 V: a level shifter between them, such as a PCA9306 breakout (with pull-ups on both sides):

| Signal | Pico | PCA9306 | nRF7002 DK |
|---|---|---|---|
| SDA | GP4 (pin 6) | SDA2 / SDA1 | P1.02 (Arduino SDA) |
| SCL | GP5 (pin 7) | SCL2 / SCL1 | P1.03 (Arduino SCL) |
| Reference | 3V3(OUT) (pin 36) | VREF2 / VREF1 | VDD (1.8 V) |
| Ground | GND (pin 38) | GND | GND |

```
west build --sysbuild -b rpi_pico/rp2040/mcuboot -d build-stress-gw twixt/tests/stress -- -DMCU=gw
west build -b nrf7002dk/nrf5340/cpuapp -d build-stress-b twixt/tests/stress -- -DMCU=b
west flash -d build-stress-b
```

The Pico goes on as below. Its USB CDC-ACM ports, `/dev/serial/by-id/usb-twixt_twixt_pico-ifNN`: `-if00` the log, `-if02` MCUmgr, `-if04` the gateway's own shell, `-if06` board a, `-if08` board b. The DK logs on its VCOM.

With twister (`--device-testing`), b on the DK is the DUT and the gateway on the Pico a required device (`testcase.yaml`): twister flashes both, the Pico over MCUmgr with twixt's `smp` runner (`scripts/runners/smp.py`), and the test runs the matrix of b and gw. The hardware map: `../../scripts/twixt-hw.example.yaml`, with your DK's J-Link serial number and VCOM:

```
west twister --device-testing --hardware-map twixt-hw.yaml \
	-p nrf7002dk/nrf5340/cpuapp -p rpi_pico/rp2040/mcuboot -T twixt/tests/stress
```

Without twister, on boards flashed by hand:

```
GW=/dev/serial/by-id/usb-twixt_twixt_pico-
pytest twixt/tests/stress/pytest --ports b=${GW}if08,gw=${GW}if04 --seconds 20
```

A second nRF7002 DK joins as board a: built with `-DMCU=a`, and `a=${GW}if06` in `--ports`.

## The Pico under MCUboot

The Pico runs under MCUboot (`rpi_pico/rp2040/mcuboot`, `sysbuild.conf`), so that it updates itself over USB, e.g. in CI. The first time, MCUboot and the image go on together as a UF2: hold BOOTSEL while plugging the Pico in, and copy `build-stress-gw.uf2` to the RPI-RP2 drive:

```
python3 zephyr/scripts/build/mergehex.py --output-bin -o build-stress-gw.bin \
	build-stress-gw/mcuboot/zephyr/zephyr.hex build-stress-gw/stress/zephyr/zephyr.signed.hex
python3 zephyr/scripts/build/uf2conv.py -c -f 0xe48bff56 -b 0x10000000 -o build-stress-gw.uf2 build-stress-gw.bin
```

After that, every new image over MCUmgr, on the Pico's MCUmgr port (`-if02`), as twister's `smp` runner does:

```
smpmgr --port /dev/serial/by-id/usb-twixt_twixt_pico-if02 --timeout 60 upgrade --confirm build-stress-gw/stress/zephyr/zephyr.signed.bin
```

`--confirm` keeps the new image; without it MCUboot goes back to the old one at the next reset, unless the image is confirmed (`smpmgr image state-write --confirm`) once it runs.
