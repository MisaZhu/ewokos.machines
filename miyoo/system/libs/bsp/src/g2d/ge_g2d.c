/*
 * ge_g2d.c - SigmaStar SSD202D GE (2D graphics engine) hardware back end
 * for the EwokOS miyoo bsp_g2d layer, ported from the retired custom
 * miyoo g2dd native path (proven on real Miyoo hardware: synchronous
 * FillRect and same-size BitBlt jobs).
 *
 * Register map (recovered from the vendor libmi_gfx.so / mi_gfx.ko):
 *   ctrl        0x000   ENABLE=bit0, ABL=bit2, DFB=bit10
 *   ctrl1       0x004   CLK_EN=bit15
 *   status      0x01c   BUSY=bit0
 *   irq         0x078   clear = 3<<10
 *   abl_coef    0x044   DFB alpha blend coefficient mode: 1 = constant
 *   abl_const   0x04c   constant alpha, replicated in both bytes
 *   src_base    0x080   MIU bus address (lo/hi 16-bit words)
 *   dst_base    0x098   MIU bus address
 *   src_pitch   0x0c0   bytes per row
 *   dst_pitch   0x0cc   bytes per row
 *   colorfmt    0x0d0   src fmt | dst fmt<<8, ARGB8888 = 0x0f
 *   clip        0x154.. clip left/right/top/bottom (inclusive)
 *   rot         0x164   rotation, no recovered sequence - kept 0
 *   cmd         0x180   job kick: RECTFILL = 3<<4, BITBLT = 4<<4
 *   x0..y2      0x1a0.. dst rect x0,y0,x1,y1 (inclusive), src origin x2,y2
 *   src_w/h     0x1b8.. src surface dimensions
 *   color       0x1c0   fill color
 *
 * The GE sits on the RIU bus and takes 16-bit word accesses only; 32-bit
 * registers are written as two halfwords.  Buffer addresses use the
 * MIU-visible bus view: the ARM physical address minus the 0x20000000
 * DRAM window base.
 *
 * GE-eligible canvases are physically contiguous shm slabs or dma
 * memory, both mapped NOCACHE in every process (kernel shm.c maps
 * IPC_CONTIG segments PTE_ATTR_NOCACHE), so no ARM cache maintenance is
 * needed around a dispatch.
 *
 * Synchronous dispatch: wait idle, program, kick CMD, wait idle again.
 * A timeout returns -1 and the operation is NOT replayed on the cpu
 * (a wedged engine may still own or have partially written the
 * destination).
 */

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sysinfo.h>
#include <ewoksys/sys.h>
#include <ewoksys/mmio.h>
#include "ge_g2d.h"

/* GE lives inside the miyoo MMIO window (_sys_info.mmio.phy_base =
 * 0x1f000000, 16 MB - see machines/miyoo/kernel/bsp/hw_info_arch.c), so
 * the userland mmio_map() window already covers it. */
#define MIYOO_MMIO_PHY_BASE  0x1f000000U
#define SSD20XD_GE_PHY_BASE  0x1f281200U

/* ARM physical base of the DRAM window: MIU bus address = phy - base */
#define MIYOO_MIU_PHY_BASE   0x20000000U

#define GE_REG_CTRL          0x000U
#define GE_REG_CTRL1         0x004U
#define GE_REG_STATUS        0x01cU
#define GE_REG_IRQ           0x078U
#define GE_REG_ABL_COEF      0x044U
#define GE_REG_ABL_CONST     0x04cU
#define GE_REG_SRC_BASE      0x080U
#define GE_REG_DST_BASE      0x098U
#define GE_REG_SRC_PITCH     0x0c0U
#define GE_REG_DST_PITCH     0x0ccU
#define GE_REG_COLORFMT      0x0d0U
#define GE_REG_CLIP_LEFT     0x154U
#define GE_REG_CLIP_RIGHT    0x158U
#define GE_REG_CLIP_TOP      0x15cU
#define GE_REG_CLIP_BOTTOM   0x160U
#define GE_REG_ROT           0x164U
#define GE_REG_CMD           0x180U
#define GE_REG_X0            0x1a0U
#define GE_REG_Y0            0x1a4U
#define GE_REG_X1            0x1a8U
#define GE_REG_Y1            0x1acU
#define GE_REG_X2            0x1b0U
#define GE_REG_Y2            0x1b4U
#define GE_REG_SRC_WIDTH     0x1b8U
#define GE_REG_SRC_HEIGHT    0x1bcU
#define GE_REG_COLOR         0x1c0U

#define GE_CTRL_ENABLE       (1U << 0)
#define GE_CTRL_ABL          (1U << 2)
#define GE_CTRL_DFB          (1U << 10)
#define GE_CTRL1_CLK_EN      (1U << 15)

#define GE_STATUS_BUSY       (1U << 0)
#define GE_IRQ_CLR_MASK      (3U << 10)

#define GE_FMT_ARGB8888      0x000fU

#define GE_CMD_RECTFILL      (3U << 4)
#define GE_CMD_BITBLT        (4U << 4)

/* wait-idle timeouts in microseconds: 2 ms for the engine to go idle
 * before programming, 20 ms for a job to complete (a 640x480 blit is
 * far below that) */
#define GE_WAIT_IDLE_US      2000
#define GE_WAIT_JOB_US       20000

static int _ok = 0;
static ewokos_addr_t _ram_base = 0;   /* DRAM window (MIU translation) */
static ewokos_addr_t _ram_top = 0;    /* end of physical RAM */

static inline ewokos_addr_t ge_reg_addr(uint32_t reg) {
    return _mmio_base + (SSD20XD_GE_PHY_BASE - MIYOO_MMIO_PHY_BASE) + reg;
}

static inline uint16_t ge_read16(uint32_t reg) {
    return *((volatile uint16_t*)ge_reg_addr(reg));
}

static inline void ge_write16(uint32_t reg, uint16_t value) {
    *((volatile uint16_t*)ge_reg_addr(reg)) = value;
}

static inline void ge_write32(uint32_t reg, uint32_t value) {
    ge_write16(reg, (uint16_t)(value & 0xffffU));
    ge_write16(reg + 2U, (uint16_t)((value >> 16) & 0xffffU));
}

static int32_t ge_wait_idle(uint32_t timeout_us) {
    while (timeout_us > 0) {
        if ((ge_read16(GE_REG_STATUS) & GE_STATUS_BUSY) == 0)
            return 0;
        usleep(1);
        timeout_us--;
    }
    return -1;
}

static void ge_irq_clear(void) {
    ge_write16(GE_REG_IRQ, GE_IRQ_CLR_MASK);
    ge_write16(GE_REG_IRQ, 0);
}

/* common per-job state: engine on, clock on, no rotation, ARGB8888 on
 * both surfaces, clip at the surface bounds, irq clear.  use_alpha != 0
 * turns the DFB blend stage on with a global constant alpha (the
 * sequence recovered for the retired g2dd: CTRL ABL|DFB, ABL_COEF = 1
 * selects constant-alpha mode, ABL_CONST = alpha in both bytes). */
static void ge_reset_state(int32_t w, int32_t h, uint8_t use_alpha, uint8_t alpha) {
    uint16_t ctrl = GE_CTRL_ENABLE;
    uint16_t ctrl1 = ge_read16(GE_REG_CTRL1);

    if (use_alpha != 0) {
        ctrl |= GE_CTRL_ABL | GE_CTRL_DFB;
        ge_write16(GE_REG_ABL_COEF, 0x0001U);
        ge_write16(GE_REG_ABL_CONST, (uint16_t)alpha | ((uint16_t)alpha << 8));
    }
    else {
        ge_write16(GE_REG_ABL_COEF, 0);
        ge_write16(GE_REG_ABL_CONST, 0);
    }

    ctrl1 |= GE_CTRL1_CLK_EN;
    ge_write16(GE_REG_CTRL1, ctrl1);
    ge_write16(GE_REG_CTRL, ctrl);
    ge_write16(GE_REG_CTRL + 2U, 0);
    ge_write16(GE_REG_ROT, 0);
    ge_write16(GE_REG_COLORFMT, GE_FMT_ARGB8888 | (GE_FMT_ARGB8888 << 8));
    ge_write16(GE_REG_CLIP_LEFT, 0);
    ge_write16(GE_REG_CLIP_RIGHT, (uint16_t)(w - 1));
    ge_write16(GE_REG_CLIP_TOP, 0);
    ge_write16(GE_REG_CLIP_BOTTOM, (uint16_t)(h - 1));
    ge_irq_clear();
}

int ge_g2d_phy_valid(ewokos_addr_t phy, size_t bytes) {
    ewokos_addr_t end;

    if (bytes == 0 || _ram_top <= _ram_base)
        return 0;
    end = phy + bytes;
    if (end <= phy)                 /* wrap */
        return 0;
    if (phy < _ram_base || end > _ram_top)
        return 0;
    return 1;
}

uint32_t ge_g2d_miu(ewokos_addr_t phy) {
    if (phy < MIYOO_MIU_PHY_BASE)
        return 0;
    return (uint32_t)(phy - MIYOO_MIU_PHY_BASE);
}

int ge_g2d_init(void) {
    sys_info_t sysinfo;

    if (_ok)
        return 0;
    if (_mmio_base == 0 && mmio_map() == 0)
        return -1;

    if (sys_get_sys_info(&sysinfo) != 0)
        return -1;
    _ram_base = sysinfo.phy_offset;
    _ram_top = sysinfo.phy_offset + sysinfo.total_phy_mem_size;
    if (_ram_top <= _ram_base)
        return -1;

    /* probe: the engine must answer an idle poll */
    if (ge_wait_idle(GE_WAIT_IDLE_US) != 0)
        return -1;
    _ok = 1;
    return 0;
}

int ge_g2d_ready(void) {
    return _ok;
}

int ge_g2d_fill(uint32_t miu, int32_t w, int32_t h,
                int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                uint32_t argb) {
    if (!_ok || w <= 0 || h <= 0 || x0 < 0 || y0 < 0 ||
        x1 <= x0 || y1 <= y0 || x1 > w || y1 > h)
        return -1;

    if (ge_wait_idle(GE_WAIT_IDLE_US) != 0)
        return -1;

    ge_reset_state(w, h, 0, 0);
    ge_write32(GE_REG_DST_BASE, miu);
    ge_write16(GE_REG_DST_PITCH, (uint16_t)(w * 4));
    ge_write32(GE_REG_COLOR, argb);
    ge_write16(GE_REG_X0, (uint16_t)x0);
    ge_write16(GE_REG_Y0, (uint16_t)y0);
    ge_write16(GE_REG_X1, (uint16_t)(x1 - 1));
    ge_write16(GE_REG_Y1, (uint16_t)(y1 - 1));
    ge_write16(GE_REG_CMD, GE_CMD_RECTFILL);
    return ge_wait_idle(GE_WAIT_JOB_US);
}

static int ge_blit(uint32_t src_miu, int32_t src_w, int32_t src_h,
                int32_t sx, int32_t sy,
                uint32_t dst_miu, int32_t dst_w, int32_t dst_h,
                int32_t dx, int32_t dy, int32_t w, int32_t h,
                uint8_t use_alpha, uint8_t alpha) {
    if (!_ok || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
        sx < 0 || sy < 0 || dx < 0 || dy < 0 || w <= 0 || h <= 0 ||
        sx + w > src_w || sy + h > src_h ||
        dx + w > dst_w || dy + h > dst_h)
        return -1;

    if (ge_wait_idle(GE_WAIT_IDLE_US) != 0)
        return -1;

    ge_reset_state(dst_w, dst_h, use_alpha, alpha);
    ge_write32(GE_REG_SRC_BASE, src_miu);
    ge_write16(GE_REG_SRC_PITCH, (uint16_t)(src_w * 4));
    ge_write32(GE_REG_DST_BASE, dst_miu);
    ge_write16(GE_REG_DST_PITCH, (uint16_t)(dst_w * 4));
    ge_write16(GE_REG_SRC_WIDTH, (uint16_t)src_w);
    ge_write16(GE_REG_SRC_HEIGHT, (uint16_t)src_h);
    ge_write16(GE_REG_X0, (uint16_t)dx);
    ge_write16(GE_REG_Y0, (uint16_t)dy);
    ge_write16(GE_REG_X1, (uint16_t)(dx + w - 1));
    ge_write16(GE_REG_Y1, (uint16_t)(dy + h - 1));
    ge_write16(GE_REG_X2, (uint16_t)sx);
    ge_write16(GE_REG_Y2, (uint16_t)sy);
    ge_write16(GE_REG_CMD, GE_CMD_BITBLT);
    return ge_wait_idle(GE_WAIT_JOB_US);
}

int ge_g2d_blit(uint32_t src_miu, int32_t src_w, int32_t src_h,
                int32_t sx, int32_t sy,
                uint32_t dst_miu, int32_t dst_w, int32_t dst_h,
                int32_t dx, int32_t dy, int32_t w, int32_t h) {
    return ge_blit(src_miu, src_w, src_h, sx, sy,
                   dst_miu, dst_w, dst_h, dx, dy, w, h, 0, 0);
}

int ge_g2d_blit_alpha(uint32_t src_miu, int32_t src_w, int32_t src_h,
                int32_t sx, int32_t sy,
                uint32_t dst_miu, int32_t dst_w, int32_t dst_h,
                int32_t dx, int32_t dy, int32_t w, int32_t h,
                uint8_t alpha) {
    return ge_blit(src_miu, src_w, src_h, sx, sy,
                   dst_miu, dst_w, dst_h, dx, dy, w, h, 1, alpha);
}
