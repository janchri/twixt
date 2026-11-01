<!-- SPDX-License-Identifier: Apache-2.0 -->

# RFC: Twixt, services between MCUs over a shared I2C bus

**Status:** draft, not filed yet. The sections follow Zephyr's [RFC form](https://github.com/zephyrproject-rtos/zephyr/issues/new?template=003_rfc-proposal.yml), plus the test strategy that the [RFC guidelines](https://docs.zephyrproject.org/latest/contribute/proposals_and_rfcs.html) ask for.

## Problem Description

Many devices have several MCUs on one board, for example a radio MCU and an application MCU. They often already share an I2C bus, but Zephyr has no ready-made way for equal MCUs to call each other over it.

Today, each project builds its own protocol, and usually picks one of these:

- One fixed controller polls the others, which adds latency and a central MCU.
- An extra interrupt wire tells the controller when to poll, which costs a pin on every MCU.

Each project then also has to solve retries after lost arbitration, and how to reach a second MCU's shell or update its firmware.

## Proposed Change (Summary)

Add Twixt, a subsystem where every MCU is an I2C target at its own address and a controller when it sends. One combined I2C transfer carries a request and its response. There is no central MCU and no extra wire.

Services give requests their meaning: ping, echo, settings, a UART for shells, MCUmgr, and application services. A gateway lets stock SMP tools update every MCU over USB, UART or Bluetooth, without a shell.

Twixt is available today as an out-of-tree [Zephyr module](https://github.com/janchri/twixt) that needs no changes to Zephyr.

## Proposed Change (Detailed)

### Layers

| Layer | Job | Code |
|---|---|---|
| Services | Give requests a meaning | `subsys/twixt/service`, `drivers/serial/uart_twixt.c` |
| Clients | Use services: SMP gateway, echo load | `subsys/twixt/client` |
| Twixt | Service dispatch, request format | `subsys/twixt/twixt.c` |
| Link | One request and response per transfer, retries | `subsys/twixt/link.c` |
| I2C driver | The bus; any capable Zephyr driver | Zephyr, or `drivers/i2c` |

Applications configure it with devicetree only. Every image lists all MCUs, and `/chosen twixt,self` says which one it is:

```dts
&i2c0 {
	mcu_pico: pico@10 { compatible = "twixt,mcu"; reg = <0x10>; };
	mcu_nrf: nrf@11 { compatible = "twixt,mcu"; reg = <0x11>; };
};

/ {
	chosen {
		twixt,self = &mcu_pico;
	};
};
```

### Wire format

A call is one I2C write, a repeated START, and one read:

```text
request:   service, src, flags, data...
response:  status, data...
```

For example, 0x10 sends the echo service on 0x11 the bytes `41 42`:

```text
S 22 A | 01 A 10 A 00 A 41 A 42 A | Sr 23 A | 00 A 41 A 42 N P
```

- `src` is the sender's address. `flags` is reserved: written as 0, ignored on receipt.
- `status` is 0, or an errno without its sign. 0xff is what a read returns when nobody answers, so it is never sent.
- A message holds up to `CONFIG_TWIXT_LINK_MSG_MAX` bytes, 64 by default, and every MCU on the bus must use the same value.
- Service ids are the same on every MCU. Ids from 0x80 are for applications.

### Behavior

- **A request runs at most once.** The target runs it when the read arrives, and forgets it once answered or at a STOP. A read without a request gets `EBADMSG`.
- **Services run in the I2C target interrupt,** so they must be short. Slower work is deferred: settings are saved by a work item, and an MCUmgr response comes back as a new transfer from the other MCU.
- **The link retries** lost arbitration and a busy bus for 500 ms, and an address NACK for 5 ms by default. Other errors are not retried, because the request may have run.
- **Errors say whether a request ran.** `-ENXIO`, `-EAGAIN`, `-EBUSY`, `-EIO` and `-EBADMSG` mean it did not. `-ETIMEDOUT` and `-ENOMSG` mean it may have.
- **Arbitration follows the bytes on the wire.** The lowest frame wins: the destination address decides first, then the service id, then `src`.

### Hardware without a multi-master controller

Some I2C blocks cannot detect lost arbitration, such as the nRF TWIM, or cannot be controller and target at once, such as the RP2040's DesignWare block. For these, the `twixt,i2c-mm-gpio` driver keeps the block as the target and clocks the MCU's own transfers in software on the same pins. While it sends, the MCU NACKs its address, and other MCUs retry. This costs CPU time, with interrupts locked during each bit's high phase.

### Testing without hardware

`twixt,i2c-mm-native` connects `native_sim` processes through shared memory, so a whole multi-MCU system runs in CI.

## Dependencies

- **Zephyr:** I2C controller and target APIs. No changes are needed for the module to work.
- **MCUmgr:** for the SMP service and gateway.
- **Bluetooth:** only for the Bluetooth gateway.
- **MCUboot:** only to update images.
- **Other teams:** I2C driver maintainers, for the driver questions below.

## Concerns and Unresolved Questions

1. **Driver contract.** Zephyr's I2C API doesn't guarantee what Twixt needs: being target and controller at once, reporting lost arbitration as `-EAGAIN`, reporting a STOP to the target, and answering in time or stretching SCL. Should these become documented capabilities?
2. **STOP detection on the nRF.** Zephyr's TWIS driver doesn't report STOP, so the GPIO driver infers it from the lines staying high for two bit times. That is a workaround. A real STOP event would be better.
3. **Status values.** Errno numbers depend on the libc, so they aren't a stable wire format. Twixt currently makes `native_sim` use Zephyr's minimal libc to get matching numbers. A fixed table of status values may be needed before the format is frozen.
4. **No authentication.** `src` isn't verified, which assumes a trusted bus. The Bluetooth gateway doesn't require pairing yet, so anyone in range can send SMP commands to every MCU. MCUboot still rejects unsigned images.
5. **Lost responses.** If a response is lost, the sender can't tell whether the request ran, so requests with side effects need their own protection. Should Twixt add request ids?
6. **Open failure on real boards.** Under two-way load between the nRF and the Pico, about 0.05% of the calls from the nRF to the Pico have ended in `-ENXIO`. The most recent 20 s run had no errors, and the cause isn't known.
7. **Bus sharing with other I2C devices.** Plain peripherals should work on the same driver instance, but this isn't tested yet.
8. **Where it belongs.** Should Twixt be a Zephyr subsystem, or stay a module listed in the manifest?

## Alternatives Considered

| Alternative | Why not |
|---|---|
| One controller polling the others | Needs a fixed central MCU, and adds polling latency and load |
| Extra interrupt wire | Costs a pin per MCU, which the bus should make unnecessary |
| SPI or UART between MCUs | Needs more wires, or a separate link per pair of MCUs |
| CAN | Needs a controller and transceivers that these boards don't have |
| Separate transfers for request and response | Another master can get in between, and a slow service would need its own polling |
| SMBus-style register access | Fits sensors, not calls between equal MCUs that each start transfers |

## Test Strategy

| Test | What it covers | Where it runs |
|---|---|---|
| `tests/link` | Link and GPIO driver on a fake bus | `native_sim` |
| `tests/services` | Services and faults, one MCU | `native_sim` |
| `tests/smp_gateway` | Gateway core with fakes: resends, late answers, no answer | `native_sim` |
| `tests/multi_mcu` | All services between two MCUs; a request runs once or not at all | `native_sim`, boards |
| `tests/stress` | Every combination of senders against every combination of targets; any error fails it | `native_sim`, boards |
| `tests/smp_update` | Real image updates with `smpmgr` | Boards: USB with twister, Bluetooth by hand |

The boards are a Raspberry Pi Pico and an nRF7002 DK, connected through a level shifter.

Not covered yet:

- Logic-analyzer checks of bus timing
- Other I2C controllers, such as STM32 I2C v2 or NXP LPI2C
- Plain I2C peripherals on the same bus
- Serial and Bluetooth gateway code in CI: these only run on the boards
