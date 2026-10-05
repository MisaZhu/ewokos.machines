/*
 * vgacon.c: 内核控制台 (kout 的第二输出汇点, uart_write 内镜像)。
 *
 * 双后端, 无串口平台上引导日志与挂死点可见:
 *   1. GOP 帧缓冲 (UEFI 机器, bootinfo fb): arch_vm 完成 fb 映射并由
 *      hw_info_arch 调 vgacon_fb_ready() 后启用 —— 现代机器 (GPD 等
 *      UEFI-only) 的 0xB8000 文本区不接显示, 唯一可见输出;
 *   2. VGA 文本 0xB8000 80x25 (BIOS 机器), boot_pml4 早期走低恒等。
 *
 * VA 0xB8000 恒等映射存在于全部三个页表阶段; fb 窗口 (X86_FB_VA,
 * PDPT[2]) 经 clone_kernel_vm 的 PDPT[2] 引用共享对所有任务可见。
 */
#include <mm/mmu.h>
#include "arch.h"
#include "x86_platform.h"
#include "fb_font.h"

/* boot.S 的早期页表 (低 VA 恒等 = 物理地址); CR3 等于它说明还在重映射
 * 切换之前, 此时高 VA 尚未映射, VGA 走 boot_pml4 的低恒等; 切换后 CR3
 * 为内核表/任务表 (PDPT[2] 共享), VGA 走高 VA —— 两阶段全通。 */
extern char boot_pml4;

#define VGA_TEXT_VADDR  X86_VGA_TEXT_VADDR
#define VGA_COLS        80
#define VGA_ROWS        25
#define VGA_ATTR        0x07    /* 黑底白字 */

/* ---- GOP 帧缓冲控制台 ---- */
#define FB_GLYPH_W      8
#define FB_GLYPH_H      16
#define FB_FG           0xffe8e8e8      /* 亮灰白 */
#define FB_BG           0xff101010      /* 近黑 */

static int32_t _vga_row;
static uint32_t _vga_col;
static int32_t _vga_init;
static int32_t _vga_active = 1;

static int32_t _fb_ready;               /* hw_info_arch: fb 映射完成后置位 */
static volatile uint8_t *_fb;           /* X86_FB_VA + fb 基址 2MB 内偏移 */
static uint32_t _fb_w, _fb_h, _fb_pitch, _fb_bpp;
static uint32_t _fb_scale;              /* 高分屏 2x2 放大 */
static uint32_t _fb_cols, _fb_rows;
static uint32_t _fb_cx, _fb_cy;

static inline volatile char* vga_cell(uint32_t row, uint32_t col) {
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    uint64_t base = (cr3 == (uint64_t)(uintptr_t)&boot_pml4)
            ? (uint64_t)X86_VGA_TEXT_PHYS           /* 早期: 低恒等映射 */
            : (uint64_t)X86_VGA_TEXT_VADDR;         /* 切换后: 高 VA */
    return (volatile char*)(base + (row * VGA_COLS + col) * 2);
}

/* ---- VGA 文本后端 (BIOS 机器) ---- */
static void vga_scroll(void) {
    volatile char* dst = vga_cell(0, 0);
    volatile char* src = vga_cell(1, 0);
    for (uint32_t i = 0; i < (VGA_ROWS - 1) * VGA_COLS * 2; ++i) {
        dst[i] = src[i];
    }
    for (uint32_t c = 0; c < VGA_COLS; ++c) {
        vga_cell(VGA_ROWS - 1, c)[0] = ' ';
        vga_cell(VGA_ROWS - 1, c)[1] = VGA_ATTR;
    }
}

void vgacon_init(void) {
    for (uint32_t r = 0; r < VGA_ROWS; ++r) {
        for (uint32_t c = 0; c < VGA_COLS; ++c) {
            vga_cell(r, c)[0] = ' ';
            vga_cell(r, c)[1] = VGA_ATTR;
        }
    }
    _vga_row = 0;
    _vga_col = 0;
    _vga_init = 1;
}

void vgacon_handoff(void) {
    /* 用户态接管后停写: 屏幕保留最后一屏内核日志 */
    _vga_active = 0;
}

/* ---- GOP 帧缓冲后端 ---- */
static inline void fb_px(uint32_t x, uint32_t y, uint32_t c) {
    volatile uint8_t *p = _fb + (uint64_t)y * _fb_pitch + x * (_fb_bpp / 8);
    if (_fb_bpp == 32) {
        p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8);
        p[2] = (uint8_t)(c >> 16); p[3] = 0xff;
    } else {
        p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)(c >> 16);
    }
}

static void fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) {
    for (uint32_t yy = 0; yy < h; ++yy) {
        for (uint32_t xx = 0; xx < w; ++xx) {
            fb_px(x + xx, y + yy, c);
        }
    }
}

static void fb_fill_screen(void) {
    for (uint32_t y = 0; y < _fb_h; ++y) {
        for (uint32_t x = 0; x < _fb_w; ++x) {
            fb_px(x, y, FB_BG);
        }
    }
}

static void fb_glyph(uint32_t col, uint32_t row, char c, uint32_t color) {
    if (c < 0x20 || c > 0x7e) {
        c = '.';
    }
    const uint8_t *g = fb_font[c - 0x20];
    uint32_t x0 = col * FB_GLYPH_W * _fb_scale;
    uint32_t y0 = row * FB_GLYPH_H * _fb_scale;
    for (uint32_t ry = 0; ry < FB_GLYPH_H; ++ry) {
        uint8_t bits = g[ry];
        for (uint32_t rx = 0; rx < FB_GLYPH_W; ++rx) {
            uint32_t v = (bits & (0x80 >> rx)) ? color : FB_BG;
            if (_fb_scale == 1) {
                fb_px(x0 + rx, y0 + ry, v);
            } else {
                fb_rect(x0 + rx * 2, y0 + ry * 2, 2, 2, v);
            }
        }
    }
}

static void fb_cursor(int32_t show) {
    uint32_t c = show ? FB_FG : FB_BG;
    uint32_t x = _fb_cx * FB_GLYPH_W * _fb_scale;
    uint32_t y = (_fb_cy * FB_GLYPH_H + FB_GLYPH_H - 2) * _fb_scale;
    fb_rect(x, y, FB_GLYPH_W * _fb_scale, 2 * _fb_scale, c);
}

static void fb_scroll(void) {
    uint32_t cell_bytes = FB_GLYPH_H * _fb_scale * _fb_pitch;
    volatile uint8_t *dst = _fb;
    volatile uint8_t *src = _fb + cell_bytes;
    for (uint32_t i = 0; i < cell_bytes * (_fb_rows - 1); ++i) {
        dst[i] = src[i];
    }
    fb_rect(0, (_fb_rows - 1) * FB_GLYPH_H * _fb_scale,
            _fb_w, FB_GLYPH_H * _fb_scale, FB_BG);
}

static void fb_putchar(char c) {
    fb_cursor(0);
    if (c == '\n') {
        _fb_cx = 0;
        _fb_cy++;
    } else if (c == '\r') {
        _fb_cx = 0;
    } else {
        fb_glyph(_fb_cx, _fb_cy, c, FB_FG);
        if (++_fb_cx >= _fb_cols) {
            _fb_cx = 0;
            _fb_cy++;
        }
    }
    if (_fb_cy >= _fb_rows) {
        fb_scroll();
        _fb_cy = _fb_rows - 1;
    }
    fb_cursor(1);
}

/* hw_info_arch.c: fb 映射 (arch_vm) 完成后调用, 启用 GOP 控制台 */
void vgacon_fb_ready(void) {
    if (x86_platform_data.fb.pitch == 0 || x86_platform_data.fb.height == 0 ||
            x86_platform_data.fb.bpp < 16 || x86_platform_data.fb.width == 0) {
        return;
    }
    _fb_w = x86_platform_data.fb.width;
    _fb_h = x86_platform_data.fb.height;
    _fb_pitch = x86_platform_data.fb.pitch;
    _fb_bpp = x86_platform_data.fb.bpp;
    _fb_scale = (_fb_w >= 1600) ? 2 : 1;
    _fb_cols = _fb_w / (FB_GLYPH_W * _fb_scale);
    _fb_rows = _fb_h / (FB_GLYPH_H * _fb_scale);
    _fb = (volatile uint8_t *)(uintptr_t)X86_FB_VA +
            (uint32_t)(x86_platform_data.fb.phy_base & 0x1FFFFF);
    _fb_cx = _vga_col;                       /* 续写文本阶段的列位置 */
    _fb_cy = 0;
    fb_fill_screen();
    _fb_ready = 1;
}

/* ---- 分发 ---- */
static void vga_text_write(const char* s, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        char c = s[i];
        if (c == '\n') {
            _vga_col = 0;
            _vga_row++;
        }
        else if (c == '\r') {
            _vga_col = 0;
        }
        else {
            vga_cell(_vga_row, _vga_col)[0] = c;
            vga_cell(_vga_row, _vga_col)[1] = VGA_ATTR;
            if (++_vga_col >= VGA_COLS) {
                _vga_col = 0;
                _vga_row++;
            }
        }
        if (_vga_row >= VGA_ROWS) {
            vga_scroll();
            _vga_row = VGA_ROWS - 1;
        }
    }
}

void vgacon_write(const char* s, uint32_t len) {
    if (!_vga_active) {
        return;
    }
    if (!_vga_init) {
        vgacon_init();
    }
    if (!_fb_ready) {
        /* 惰性激活: fb 窗口仅在内核 VM (PDPT[2]) 映射, CR3 切换前写入
         * 会踩未映射页 —— boot_pml4 阶段保持 VGA 文本后端, 切换后首个
         * kout 激活 GOP (真机 UEFI 唯一可见输出) */
        uint64_t cr3;
        __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
        if (cr3 != (uint64_t)(uintptr_t)&boot_pml4 && 0) {   /* BISECT: lazy 禁用 */
            vgacon_fb_ready();
        }
    }
    if (_fb_ready) {
        for (uint32_t i = 0; i < len; ++i) {
            fb_putchar(s[i]);
        }
        return;
    }
    vga_text_write(s, len);
}
