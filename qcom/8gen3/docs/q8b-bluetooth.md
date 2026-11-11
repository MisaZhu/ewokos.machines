# Q8B Bluetooth interface (raspi5-compatible)

The QCNFA765 is a combo module: Wi-Fi is a PCIe function (owned by `wlan`)
and Bluetooth is a USB function (claimed by `usbhostd`). The Bluetooth
*daemon*, HID stack and desktop tools are the raspi5 ones and are shared
unchanged; only the HCI transport in `libbsp` and the USB firmware download
are Q8B code.

| Item | raspi5 | Q8B |
|---|---|---|
| Device node | `/dev/bt0` (0666) | same |
| Daemon | `system/gui/drivers/btd` | same source, `HW=q8b` build |
| HCI transport | 16550 UART + `BT_ON` | USB bulk/interrupt through `usbhostd` |
| Firmware | CYW43455 patchram downloaded by `btd` | QCA DFU + HCI reset in `usbhostd` before `btd` sees the stream |
| Transport contract | `bt/bsp_bt.h` | same header, same seven hooks |
| Event-driven RX | `bsp_bt_wait_readable` returns 1/0 from the UART IRQ | returns -1; `btd` keeps its history polling cadence |
| Upper tools | `xbt`, `hid_moused`/`hid_keybd`/`hid_joystickd` in `bt` mode | same binaries and arguments |

Because the daemon and the HID subscribers are shared, the device surface an
application sees is identical on both boards.

## Device API

`/dev/bt0` speaks the same plain `key=value` text protocol as raspi5
(`xbt` parses it directly, not as JSON):

```
devcmd /dev/bt0 state          # powered/ready/scanning/devices/pending/le_*
devcmd /dev/bt0 devices        # discovery cache, "devices_done count=N"
devcmd /dev/bt0 known          # stored bonds
devcmd /dev/bt0 scan [secs]
devcmd /dev/bt0 connect <bdaddr>
devcmd /dev/bt0 pair <bdaddr> [pin]
devcmd /dev/bt0 unpair <bdaddr>
devcmd /dev/bt0 disconnect <bdaddr|handle>
devcmd /dev/bt0 open | close
```

`open`/`close`, `scan`, `connect`, `pair`, `unpair` and `disconnect` are
fire-and-forget and answer `*_begin`/`*_ok` at once; progress is reported
through the `state`/`devices` polls and the read-side event stream
(`power_on`, `scan_done`, `connected`, `bonded`, ...). The same node is the
subscriber channel: `hid_moused`, `hid_keybd` and `hid_joystickd` open it in
`bt` mode and publish `/dev/mouse1`, `/dev/keyb1` and `/dev/js1`.

## Boot wiring

`etc/xwin/init.rd` starts the raspi5 stack, plus the X input processes that
consume both the USB and the Bluetooth HID nodes:

```
@/bin/ipcserv /drivers/q8b/btd           /dev/bt0
@/bin/ipcserv /drivers/q8b/hid_moused    /dev/mouse1 /dev/bt0 bt
@/bin/ipcserv /drivers/q8b/hid_keybd     /dev/keyb1  /dev/bt0 bt
@/bin/ipcserv /drivers/q8b/hid_joystickd /dev/js1    /dev/bt0 bt
...
@/bin/bgrun /sbin/x/xmouse /dev/mouse0
@/bin/bgrun /sbin/x/xmouse /dev/mouse1
@/bin/bgrun /sbin/x/xim_none /dev/keyb0
@/bin/bgrun /sbin/x/xim_none /dev/keyb1
@/bin/bgrun /sbin/x/xim_none /dev/js1
```

The subscriber nodes exist before a device is paired, so the X input
processes open them at boot and simply stay idle until a BT keyboard, mouse
or gamepad connects. USB input (`mouse0`/`keyb0`) remains wired in parallel;
raspi5 instead routes X exclusively to `mouse1`/`keyb1`. Because two pointer
devices now feed one server, `xmouse` encodes a wheel event relative with a
zero delta instead of re-sending its own last position, so scrolling never
moves the cursor (or routes to the wrong window) when the other mouse or the
touch panel was the last one to move it.

## Platform notes

- **Coexistence gate.** The USB and PCIe functions share the module's E-key
  rail, and `pcied` cold-cycles it before WCN startup. `bsp_bt_init()` in
  `q8b/system/libs/bsp/src/bsp_bt.c` therefore returns -1 until `wlan` has
  published a valid MAC; `btd` retries every three seconds and then runs the
  deferred QCA setup. `usbhostd` claims the USB interface at enumeration but
  does not download firmware from its probe path.
- **No rail power-down.** `bsp_bt_power_off()` (the `close` verb) only drops
  the HCI session; the 3.3 V rail is shared with Wi-Fi and must stay up. A
  later `open` re-runs the HCI bring-up without a rail power cycle. This is
  the one contract hook whose Q8B behaviour differs from the raspi5 UART
  transport (which pulls `BT_ON` low).
- **Firmware.** `bsp_bt_firmware()` returns no image on Q8B, so `btd`'s
  Broadcom patchram path is skipped. `bsp_usbbt.c` performs the vendor DFU
  (`rampatch_usb_*.bin`, `nvm_usb_*.bin` from `/etc/firmware/qca/`) and an
  HCI reset before the stream is exposed.
- **LE/HOGP vs classic HIDP**, bonding, discovery and the HID report
  translation all live in the shared daemon, so behaviour matches raspi5.

## Verification

- `machines/q8b/system/libs/bsp/tests/run-usbbt.sh` builds and runs the USB
  transport and `bsp_bt` client host tests (framing, DFU validation, firmware
  selection, HCI reset, ACL bounds, receive timeout/flush); both pass.
- `btd` and `hid_joystickd` were cross-built for `HW=q8b` against
  `system/build_aarch64/q8b/lib` (`libhid`, `libbsp`, `libewoksys`) to confirm
  the shared sources and the new build entry link on this machine.
