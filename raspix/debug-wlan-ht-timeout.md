# [OPEN] wlan-ht-timeout

## Symptoms
- `HT Avail timeout (1000000): clkctl 0x50`
- `clock not available after firmware download`
- `wlan: probe failed -1`

## Scope
- `raspix/system/drivers/wlan/brcm/brcm.c`
- `raspix/system/drivers/wlan/sdio/*`

## Hypotheses
1. Firmware is downloaded successfully, but the driver requests `CLK_AVAIL` before the dongle firmware has reached its ready/shared-memory stage.
2. The 32 kHz / WL_REG_ON / KSO bring-up sequence is incomplete on this platform, so HT clock never transitions from ALP-only state.
3. SDIO CCCR / chip clock CSR accesses after firmware download are too early or require an additional settling delay on Zero 2 W.
4. The driver skips an upstream-style readiness handshake after `brcmf_chip_set_active()`, causing a false timeout even though firmware is alive a bit later.
5. A board-specific power/reset timing issue exists outside the firmware blob itself, because the embedded Zero 2 W NVRAM already matches upstream values.

## Evidence Plan
- Add instrumentation around:
  - firmware download start/end
  - ARM/core activation
  - post-download `CLK_AVAIL` request
  - `CHIPCLKCSR`, mailbox, and shared-memory readiness state
- Reproduce on device and compare the timeline.

## Status
- Bootstrap complete
- Instrumentation patch applied in `system/drivers/wlan/brcm/brcm.c`
- Reproduction logs received: `htclk-enter` showed `clkcsr=0x40`, timeout reached `clkcsr=0x50`, but `hmb=0x00000000` remained unchanged
- Hypotheses 1 and 4 strengthened: firmware bring-up had not completed its ready/shared handshake before HT clock promotion
- Minimal fix applied: wait for `brcmf_sdio_wait_fw_ready()` immediately after `brcmf_chip_set_active()`
- Post-fix logs showed `firmware ready timeout` with `shared=0xff5600a9`, which matches the NVRAM token pattern rather than a real shared-memory pointer
- Root-cause hypothesis strengthened: `ARM_CM3` activation path dropped `rstvec`, so firmware never jumped to its entry point and never replaced the tail token with `sdpcm_shared`
- Minimal fix applied in `chip.c`: pass `rstvec` into `brcmf_chip_cm3_set_active()` and down to `brcmf_sdio_buscore_activate(rstvec)`
- Additional instrumentation applied in `chip.c`: `dbg[buscore-activate]` now prints `rstvec` and RAM address-0 readback to confirm the device is really running the new CM3 activation path
- New evidence falsified part of the previous hypothesis: `dbg[buscore-activate]` showed `rstvec=0`, so the CM3 path was not dropping a non-zero vector; the selected vector itself was wrong
- New root-cause hypothesis: BCM43430/CYW43439 CM3 firmware stores the real entry point in `fw[1]` while `fw[0]` is zero/reserved; using `fw[0]` prevents firmware startup
- Minimal fix applied in `brcm.c`: add `brcmf_sdio_select_rstvec()` and select `fw[1]` for `43430/43439 + CM3` when `fw[0]==0 && fw[1]!=0`
- Post-fix logs falsified that hypothesis too: writing `0x22c5` to address 0 did not boot firmware, and upstream Zephyr's validated Zero 2 W path does not wait for firmware-ready before requesting HT clock
- New working hypothesis: the closest upstream-compatible bring-up should keep address-0 behavior unchanged for `rambase==0`, request HT immediately after CM3 release, and use `WL_REG_ON` pulse timing closer to the known-good `10 ms low / 250 ms high`
- Minimal fix applied:
  - reverted the experimental `fw[1]` reset-vector selection
  - reverted the pre-HT `brcmf_sdio_wait_fw_ready()` gate
  - changed `main.c` WLAN reset pulse from `100 ms low / 100 ms high` to `10 ms low / 250 ms high`
- Awaiting post-fix verification on device
