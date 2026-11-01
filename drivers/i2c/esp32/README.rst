Classic ESP32
=============

``twixt,esp32-i2c-target`` supplies the target callbacks of a
``twixt,i2c-mm-gpio`` device. The existing software controller supplies arbitration,
clock synchronization, stretching waits and recovery. The ESP32 port supplies
fast GPIO access and a cycle counter, and an optional target handoff keeps the
peripheral alive while routing SDA output to GPIO during controller transfers.
Other platform ports retain their existing unregister/restore behavior.

The backend uses Espressif LL interfaces provided by Zephyr's HAL module. It
owns its I2C peripheral; the standard ESP32 I2C driver must not also own it.
There are no ESP-IDF driver patches, private driver structures or FreeRTOS calls.
Twixt's existing link, protocol, services and shell run above the backend.

Target response timing
----------------------

Classic ESP32 cannot stretch SCL in hardware. GPIO edge callbacks recognize the
repeated START following received request data, then pull SCL low at the next
falling edge. While holding SCL the backend drains RX, calls the standard Zephyr
byte target callbacks to prepare a response, and fills the TX FIFO. It then
releases SCL before the controller clocks the read address. The I2C interrupt
drains long writes, refills long reads, and reports STOP. Callbacks run in ISR
context, as on Twixt's other hardware targets; responses must not block.

Zephyr snapshots GPIO interrupt flags before invoking callbacks. The boundary
callback therefore rechecks newly pending SCL edges after sampling the levels,
without clearing them. This prevents a data edge delayed across a clock rise
from being mistaken for a repeated START. It clamps SCL immediately when armed,
and avoids FIFO reads on ordinary clock edges.

Both GPIO and I2C interrupts run at priority 3 on the sole enabled CPU, preventing
one from interrupting the other's FIFO/state changes. The backend refuses to
hand pins to the software controller while a peer's target transaction is active.
During controller transfers SDA output bypasses the target peripheral, so a peer
addressing this MCU receives a NACK and must retry.

Configuration
-------------

See ``tests/stress/boards/esp32_devkitc_esp32_procpu.overlay`` for a complete
example. The target node replaces the compatible of the chosen I2C peripheral;
its ``port`` selects hardware unit 0 or 1. Its SCL/SDA properties and the wrapper's
SCL/SDA properties must name the same pins. Both pins must be on ``gpio0`` and
configured open drain; the example uses GPIO22 for SCL and GPIO21 for SDA.
Use external pull-ups and a common ground. Set the GPIO interrupt priority to 3.

The backend requires classic ESP32, ``CONFIG_SMP=n`` and ``CONFIG_PM=n``.
``CONFIG_I2C_MM_ESP32_TARGET`` and the ``esp32`` GPIO port are selected from the
devicetree. Use the byte target API (``CONFIG_I2C_TARGET_BUFFER_MODE=n``).
The backend rejects buffer callbacks and 10-bit addresses; the wrapper rejects
controller speeds other than 100 kHz for this backend.

Limitations
-----------

- The target response path is for Twixt write/repeated-START/read exchanges;
  standalone reads without a preceding request are not supported.
- Only classic ESP32 at 100 kHz has been targeted. Other ESP32 variants need
  their own validation and may support hardware stretching instead.
- Flash writes/cache suspension during bus traffic are not supported. ISR entry
  points use IRAM, but callbacks and their dependencies can live in flash.
- Every SCL edge invokes the GPIO ISR, including other peers' traffic.
- The repeated-START detector requires at least 3 us between the SCL rise and
  SDA falling. Excessive interrupt latency can still miss the boundary.
- The backend does not hide or silently retry request failures.
