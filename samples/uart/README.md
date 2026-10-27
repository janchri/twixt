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

## Gateway on a Raspberry Pi Pico

```
west build -b rpi_pico -d build-gw twixt/samples/uart -- -DMCU=gw
```

Copy `build-gw/zephyr/zephyr.uf2` to the Pico in BOOTSEL mode. The bus is I2C0: SDA GP4, SCL GP5, with 4.7 kΩ pull-ups to 3.3 V. Each board is a USB CDC-ACM port, `/dev/serial/by-id/usb-twixt_twixt_console_gateway-if00` (a), `-if02` (b), `-if04` (c); the gateway's own shell and log is `-if06` (`twixt ping 0x10`).

The Pico's I2C driver reports lost arbitration as `-EIO` and is no target during its own transfers, so characters can get lost when the gateway and a board send at once.
