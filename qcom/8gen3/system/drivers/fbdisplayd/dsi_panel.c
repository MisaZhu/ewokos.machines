/*
 * dsi_panel.c - AMOLED panel power control for the Robo & Kala TW220.
 *
 * The Sharp RM69380 12.6" 2.5K AMOLED panel is driven in DSI video mode by
 * the UEFI GOP timing engine; EwokOS only blits pixels into the scan-out.
 * Blank-screen alone leaves the panel emitting (and the DSI link streaming),
 * so powerd's sleep path also needs the panel itself in DCS sleep-in.
 *
 * This module sends short DCS write packets through the DSI controller's
 * command DMA engine, which transmits during the video BLLP (blanking)
 * intervals - the same path Linux msm_dsi_host_cmd_tx() uses for panel
 * commands while video mode is active.  The timing engine is left running:
 * a sleeping panel ignores the stream, and GOP scan-out resumes untouched
 * on wake.  MDP/CTL power-down is deliberately out of scope.
 *
 * Register offsets and the DMA trigger sequence follow Linux
 * drivers/gpu/drm/msm/dsi/{dsi.xml.h,dsi_host.c,dsi_manager.c}.  The DSI
 * controller base (0x0ae94000, size 0x400) is the sc8280xp dsi@ae94000 node
 * from the board DT.  Everything is mapped lazily on the first command, so
 * boards that never send one (powerd is TW220-only) pay nothing.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <ewoksys/sys.h>
#include <ewoksys/syscall.h>

#include "dsi_panel.h"

#define DSI_PHY            0x0ae94000ULL
#define DSI_VA             0x58400000ULL   /* free slot: fb=0x50000000, rd=0x60000000 */
#define DSI_SIZE           0x400U

/* DSI controller registers (dsi.xml.h). */
#define DSI_REG_STATUS0    0x04U
#define DSI_STATUS0_VIDEO_MODE_ENGINE_BUSY  0x00000008U
#define DSI_REG_DMA_BASE   0x44U
#define DSI_REG_DMA_LEN    0x48U
#define DSI_REG_TRIG_DMA   0x8cU
#define DSI_REG_INTR_CTRL  0x10cU
#define DSI_IRQ_CMD_DMA_DONE                0x00000001U

/* DCS commands (mipi_display.h). */
#define DCS_DISPLAY_OFF    0x28U
#define DCS_SLEEP_IN       0x10U
#define DCS_SLEEP_OUT      0x11U
#define DCS_DISPLAY_ON     0x29U

#define DSI_TX_BUF_SIZE    64U

static volatile uint32_t* _dsi;
static uint8_t* _tx;
static uint64_t _tx_pa;
static bool _panel_sleeping;
static bool _dsi_failed;

static inline void dsi_wr(uint32_t reg, uint32_t val) {
    _dsi[reg / 4U] = val;
}

static inline uint32_t dsi_rd(uint32_t reg) {
    return _dsi[reg / 4U];
}

static inline void dsi_dsb(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Map the controller and grab a small uncached sys_dma buffer for the
 * command packets (the pool is non-cacheable, so no maintenance is needed
 * around the DMA engine reading it).  Idempotent; latches failure. */
static int dsi_init(void) {
    if (_dsi != NULL)
        return 0;
    if (_dsi_failed)
        return -1;

    if (syscall3(SYS_MEM_MAP, (ewokos_addr_t)DSI_VA,
                (ewokos_addr_t)DSI_PHY, (ewokos_addr_t)DSI_SIZE) != DSI_VA) {
        slog("q8b dsi: SYS_MEM_MAP failed pa=0x%llx va=0x%llx\n",
                (unsigned long long)DSI_PHY, (unsigned long long)DSI_VA);
        _dsi_failed = true;
        return -1;
    }
    _dsi = (volatile uint32_t*)(uintptr_t)DSI_VA;

    ewokos_addr_t va = dma_alloc(0, DSI_TX_BUF_SIZE);
    if (va == 0) {
        slog("q8b dsi: dma alloc failed\n");
        _dsi = NULL;
        _dsi_failed = true;
        return -1;
    }
    _tx_pa = (uint64_t)dma_phy_addr(0, va);
    if (_tx_pa == 0 || _tx_pa > 0xffffffffULL) {
        slog("q8b dsi: dma phy addr out of 32-bit range (0x%llx)\n",
                (unsigned long long)_tx_pa);
        dma_free(0, va);
        _dsi = NULL;
        _dsi_failed = true;
        return -1;
    }
    _tx = (uint8_t*)(uintptr_t)va;
    slog("q8b dsi: mapped ctrl va=0x%llx tx pa=0x%llx\n",
            (unsigned long long)DSI_VA, (unsigned long long)_tx_pa);
    return 0;
}

/* Send one DCS short-write (no parameter) packet via the command DMA
 * engine.  Returns 0 on CMD_DMA_DONE, -1 on timeout. */
static int dsi_dcs_short(uint8_t cmd) {
    /* Wait for a video blanking interval: the command only fits in BLLP.
     * Poll the engine-busy bit clear (<=100 ms ~ a few frames), then skip
     * past the active line like Linux dsi_wait4video_eng_busy(). */
    int i;
    for (i = 0; i < 1000; i++) {
        if (!(dsi_rd(DSI_REG_STATUS0) & DSI_STATUS0_VIDEO_MODE_ENGINE_BUSY))
            break;
        usleep(100);
    }
    usleep(3000);

    /* Ack any pending interrupt status (Linux ISR reads INTR_CTRL and
     * writes the value back). */
    dsi_wr(DSI_REG_INTR_CTRL, dsi_rd(DSI_REG_INTR_CTRL));

    /* MSM in-memory command format: header bytes swapped, bit7 = last
     * packet, bit6 clear = short write.  DCS short write no-param DI=0x05. */
    _tx[0] = cmd;
    _tx[1] = 0x00;
    _tx[2] = 0x05;
    _tx[3] = 0x80;
    dsi_dsb();

    dsi_wr(DSI_REG_DMA_BASE, (uint32_t)_tx_pa);
    dsi_wr(DSI_REG_DMA_LEN, 4U);
    dsi_wr(DSI_REG_TRIG_DMA, 1U);
    dsi_dsb();

    for (i = 0; i < 2000; i++) {
        uint32_t st = dsi_rd(DSI_REG_INTR_CTRL);
        if (st & DSI_IRQ_CMD_DMA_DONE) {
            dsi_wr(DSI_REG_INTR_CTRL, st);   /* ack */
            return 0;
        }
        usleep(100);
    }
    slog("q8b dsi: cmd 0x%02x dma timeout (intr=0x%08x)\n",
            cmd, dsi_rd(DSI_REG_INTR_CTRL));
    return -1;
}

static int panel_sleep(void) {
    if (_panel_sleeping)
        return 0;
    if (dsi_init() != 0)
        return -1;
    if (dsi_dcs_short(DCS_DISPLAY_OFF) != 0)
        return -1;
    usleep(20000);
    if (dsi_dcs_short(DCS_SLEEP_IN) != 0)
        return -1;
    usleep(120000);
    _panel_sleeping = true;
    slog("q8b dsi: panel sleep-in\n");
    return 0;
}

static int panel_wake(void) {
    if (!_panel_sleeping)
        return 0;
    if (dsi_init() != 0)
        return -1;
    if (dsi_dcs_short(DCS_SLEEP_OUT) != 0)
        return -1;
    usleep(120000);
    if (dsi_dcs_short(DCS_DISPLAY_ON) != 0)
        return -1;
    usleep(20000);
    _panel_sleeping = false;
    slog("q8b dsi: panel sleep-out\n");
    return 0;
}

char* dsi_panel_dev_cmd(int from_pid, int argc, char** argv) {
    (void)from_pid;
    if (argc <= 0 || argv == NULL || argv[0] == NULL)
        return NULL;

    if (strcmp(argv[0], "sleep") == 0)
        return strdup(panel_sleep() == 0 ? "sleeping\n" : "sleep failed\n");
    if (strcmp(argv[0], "wake") == 0)
        return strdup(panel_wake() == 0 ? "awake\n" : "wake failed\n");
    if (strcmp(argv[0], "panel") == 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "panel=%s dsi=%s\n",
                _panel_sleeping ? "sleeping" : "on",
                _dsi_failed ? "unavailable" : (_dsi ? "mapped" : "idle"));
        return strdup(buf);
    }
    return NULL;
}