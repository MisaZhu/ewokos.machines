# Q8B Wi-Fi interface (raspi5-compatible)

The Q8B `wlan` driver owns the WCN6855 (QCNFA765) PCIe endpoint. Since
2026-11 its user interface matches the Raspberry Pi 5 `wland` driver, so the
same desktop app, scripts and configuration work on both boards.

| Item | raspi5 `wland` | Q8B `wlan` |
|---|---|---|
| Device node | `/dev/wl0` | `/dev/wl0` |
| Started by | `ipcserv /drivers/raspi5/wland /dev/wl0` | `ipcserv /drivers/q8b/wlan /dev/wl0 --start` |
| Config | `/etc/wlan/network.json` | `/etc/wlan/network.json` |
| devcmd | `help log state scan list connect` | same, plus Q8B diagnostics |
| Network stack | `netd /dev/net0 /dev/wl0` | `netd /dev/net0 /dev/wl0` |

`--start` boots MHI/firmware at init; a `connect` request boots the controller
too, so the device works on a node started without `--start`.

## Configuration

`/etc/wlan/network.json` uses the pi5 schema:

```json
{
    "network":[
        {"ssid":"home","passwd":"secret","priority":1},
        {"ssid":"office","pmk":"<64 hex chars>"}
    ]
}
```

- `ssid` and one of `passwd` (8..63 chars) or `pmk` (64 hex = precomputed
  pairwise master key) are required. `pmk` wins when both exist.
- `priority` is optional (default 0). On boot the driver scans and joins the
  highest-priority entry that is visible and usable.
- A successful manual `connect` is persisted back into this file: an existing
  ssid only gets its credential refreshed, duplicate ssids are dropped, and a
  stale credential of the other kind (`pmk` vs `passwd`) is removed.
- The build installs an empty template only when the file is absent, so an
  existing configuration is never overwritten by a rootfs update.

If no JSON entry exists, the pre-pi5 profile `/etc/wlan.conf` is still read
(`ssid` LF `passphrase` LF 64-hex seed, the seed line is now optional and
ignored). This keeps SD cards provisioned before the update working. The
driver also accepts `--connect <file>` with either format.

## devcmd

```
devcmd /dev/wl0 help
devcmd /dev/wl0 log                 # recent driver log (ring buffer)
devcmd /dev/wl0 state               # JSON, same keys as pi5
devcmd /dev/wl0 scan                # "scan started" / "scan failed: -EBUSY"
devcmd /dev/wl0 list                # JSON array of scan results
devcmd /dev/wl0 connect <ssid> <passwd>
```

`state` publishes `state` (`idle`/`scanning`/`connecting`/`connected`/
`disconnected`), `ssid`, `mac`, `bssid`, `rssi`, `channel`, `type`, `auth`,
`cipher`, `ip` (always empty: netd/dhcp owns it), `scan_ready`, `reason`,
plus the q8b `link_state`/`connect_attempts` counters. `list` entries carry
`id`, `ssid`, `selected`, `bssid`, `rssi`, `channel`, `type`, `auth`,
`cipher`.

Q8B extensions: `start`, `stop`/`disconnect`, `reconnect`, `country <AA>`,
`scan-stop`, `scan-results` (text), `station` (station/WPA state), `associate
<ssid>` (associate without a supplicant) and `rings` (data-path counters).

## Behaviour

- Only WPA2-PSK/CCMP is implemented. Open, WEP, WPA1, WPA2-EAP and WPA3-SAE
  networks are listed with their security labels but cannot be joined;
  configured entries with unsupported security are skipped at scan time.
- The controller is single-owner and single-threaded. `connect` queues a
  request that the device loop executes; joining a different network tears the
  controller down and boots it again.
- Link loss or a dead control plane arms a restart ladder with 1/2/4/8/16/30 s
  backoff. A controller that never reaches WMI+DP within 60 s is restarted.
- Scanning while associated is not supported by this station implementation;
  `scan` returns `-EBUSY` then and the app keeps its cached list.
- Session entropy comes from the libc PRNG mixed with the kernel tick; the
  legacy 32-byte seed file field is no longer required.
- A truncated or malformed `network.json` is rejected before parsing. tinyjson
  never returns from an unterminated object/array/string (it grows the tree
  until the allocator fails), and a corrupt file on the persistent rootfs must
  not keep the device node from mounting. The driver logs the reason and
  continues with an empty configuration.

## Verification

`aarch64-elf-gcc` + `aarch64-elf-ld` build and link the driver against
`system/build_aarch64/q8b/lib` (including `libtinyjson.a`). Hardware bring-up
on the Q8B has not been re-run for this change; the MHI/WMI/DP/WPA paths are
unchanged apart from the join API and logging.
