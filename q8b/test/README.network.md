# Q8B TC956x network regression

The PF0 polled Ethernet driver has two RX fixes:

* `net_loop_step` defers single-task IPC delivery while polling DMA and
  updating readiness. IPC handlers run on the saved main context, so an
  unprotected read/poll upcall could re-enter the ring, overwrite `_rx_cons`,
  or corrupt the software queue halfway through a frame copy. A mutex in
  that upcall would deadlock against the suspended main context. VFS wakeups
  run **after** `ipc_enable`, since VFS may call back into this driver.
* A full software RX queue leaves the completed frame in the DMA ring.
  The next read frees space; a later poll copies the frame, rearms its
  descriptor and advances the tail. Previously poll recycled and discarded
  that frame, causing avoidable TCP retransmissions under bulk load.

`devcmd /dev/eth0 stats` reports accepted frames, descriptor drops/errors,
software backpressure, RBU observations, fatal DMA status, and ring pointers.
Backpressure and RBU are normal under bursts; they must recover when the
reader drains the queue. `errors` and `fatal` must stay zero for the bulk
acceptance run. `dropped` counts rejected DMA completions, rather than
software queue backpressure.

## Host regressions

```sh
python3 machines/q8b/test/test_net_reentry.py
python3 machines/q8b/test/test_eth_rx.py
```

Both compile the actual driver sources with small host ABI/MMIO stubs and
ASan/UBSan. The first injects 10,000 IPC upcalls and over 5,000 VFS wakeup
callbacks. The second checks full-queue retention, delivery after draining,
tail advancement, ten ring wraps, distinct payloads and bad-frame rejection.
The host RX fixture replaces ARM barriers with compiler fences; actual
PCIe/DMA ordering is covered by the board test.

Both runners accept an optional path to an older driver source. Against
the original sources, they respectively report reentry and full-queue frame
loss. Against the fixed sources, both pass.

## Board acceptance

Use the EUD MCP `eud_serial_log` tool with `action=send` for commands and
`action=read` for cached logs. Do not open a second serial reader. The
active adapter in this run is `/dev/cu.usbserial-0035E5970`; query MCP's
`action=list` rather than assuming the historical adapter name.

The diagnostic RAM rootfs includes `/drivers/q8b/net`, `/drivers/netd`,
`/sbin/sshd`, `/bin/scp` and `/bin/md5`. Init starts net, DHCP netd and sshd.
PXE loads this image from the EUD TFTP directory. The previous diagnostic
image is backed up beside it as `ROOTFS.IMG.pre-net-fix`.

```sh
make -C machines/q8b/system/libs/bsp
make -C machines/q8b/system/drivers/net
python3 machines/q8b/test/network_bulk_test.py \
  --mib 128 --rounds 3 --duplex --timeout 300 \
  --output .tmp/q8b-net/final-128m
```

The script uses unthrottled legacy SCP, without fragmentation workarounds.
Each payload has 128 MiB of reproducible pseudorandom binary data plus
137 bytes to exercise the final partial packet. Each round uploads it,
checks board MD5, downloads it, checks size/MD5/SHA-256, queries driver
errors, then requires zero-loss ping. `--duplex` adds a simultaneous upload
and download using separate board files, then repeats hashes and liveness.
Reports include commands, return codes, timings, hashes and full outputs.
All board test files live in RAM `/tmp`.

## Recorded baseline, 2026-10-22

Original ring/queue logic with statistics added: an unthrottled
4,194,441-byte upload failed after 18.09 seconds with a connection timeout.
Ping then lost all three probes; serial remained responsive. RX stayed at
80 accepted packets, zero drops, and consumer index 10; the next descriptor
was DMA-owned while most other descriptors were completed. DMA current
and tail both pointed to `0x947842a0`. This is the broken consumer state,
rather than a PHY/link failure.

With just IPC serialization, that same 4 MiB round trip passed, followed by
a 134,217,865-byte round trip with matching hashes. Bulk pressure exposed
software queue drops, which motivated the backpressure fix before final
acceptance. Subsequent intermediate-version traffic was interrupted to
deploy the final driver; its incomplete run is not a final acceptance result.

Baseline report and serial evidence are in `.tmp/q8b-net/baseline/report.json`
and `.tmp/q8b-net/serial-baseline.log`. Final-driver MD5 is
`432f5b984a1835f63784307a05b1397d`; the diagnostic image SHA-256 is
`2554c2f34b26a68340c55add60622a55c4d2116cbb78e0e49d833820afe88213`.

## Final board acceptance, 2026-10-23

The final PXE diagnostic image was booted through the EUD MCP serial cache.
The board's `/drivers/q8b/net` MD5 was
`432f5b984a1835f63784307a05b1397d`, matching the built and embedded driver.

The acceptance script ran three unthrottled 128 MiB upload/download rounds
(each payload is 134,217,865 bytes). Every board MD5 and returned SHA-256
matched `b29a66c43524e5a6759995d2e3313f16` and
`c7cee53b3da13fb2b6945233b60778cae9f3d29e02b1ba55655736facf3e543f`.
All three post-round ping checks were 5/5 with 0% loss. The final simultaneous
upload/download round also matched both hashes and passed 5/5 ping.

The final driver counters after the concurrent run were:

```text
rx=837291 tx=913009 dropped=0 errors=0 backpressure=2495 rbu=1831 fatal=00000000
```

`backpressure` and `rbu` show the ring paused and recovered under full
software-queue bursts. `dropped`, malformed-frame `errors`, and fatal DMA
status stayed zero. The full report, including timings, commands and output,
is `.tmp/q8b-net/final-128m/report.json`.
