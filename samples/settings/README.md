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
