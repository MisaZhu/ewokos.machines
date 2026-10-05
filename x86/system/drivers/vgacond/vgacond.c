/*
 * vgacond.c: 用户态控制台驱动 (/dev/vga0)。
 *
 * console_handoff 之后由 vgacond 接管屏幕, 双后端:
 *   1. GOP 帧缓冲 (UEFI 机器, sysinfo.fb): mem_map 帧缓冲后按 8x16 点阵
 *      字形绘制 —— GOP-only 机器 (GPD 等 UEFI-only 设备) 的 0xB8000 文本
 *      区不接显示, 帧缓冲是唯一可见输出; 从内核 fb 控制台的最后一行续写;
 *   2. VGA 文本 0xB8000/0xBE000000 (BIOS 机器): 内核为每个任务页表建立
 *      的用户可写恒等页, 从内核最后一屏续写。
 *
 * 无串口平台上系统输出 (登录提示、shell 回显) 经 /dev/vga0 可见;
 * 输入侧 (键盘) 待接, 当前 read 恒返回重试。
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/mmio.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>
#include <x86_platform.h>
#include "fb_font.h"

#define VGA_TEXT_VADDR  0xBE000000UL    /* 内核为所有任务映射的用户可写恒等页 */
#define VGA_COLS        80
#define VGA_ROWS        25
#define VGA_ATTR        0x07

/* GOP 帧缓冲后端 */
#define FB_BG           0xff101010      /* 与内核 vgacon 的背景一致 (续行扫描) */
#define FB_FG           0xffe8e8e8

static volatile char* _vga = NULL;      /* VGA 文本后端 */
static uint32_t _row = 0;
static uint32_t _col = 0;

static volatile uint8_t *_fb = NULL;    /* GOP 帧缓冲后端 */
static uint32_t _fb_w, _fb_h, _fb_pitch, _fb_bpp;
static uint32_t _fb_scale, _fb_cols, _fb_rows;
static uint32_t _fb_cx, _fb_cy;

static inline volatile char* vga_cell(uint32_t row, uint32_t col) {
    return _vga + (row * VGA_COLS + col) * 2;
}

/* ---- VGA 文本后端 ---- */

/* 扫描内核遗留画面, 定位最后一个非空字符之后的续写位置 */
static void vga_locate(void) {
    _row = 0;
    _col = 0;
    for (uint32_t r = VGA_ROWS; r > 0; --r) {
        for (int32_t c = VGA_COLS - 1; c >= 0; --c) {
            if (vga_cell(r - 1, c)[0] != ' ') {
                _row = r - 1;
                _col = c + 1;
                if (_col >= VGA_COLS) {
                    _col = 0;
                    _row++;
                }
                return;
            }
        }
    }
}

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

static void vga_write(const char* s, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        char c = s[i];
        if (c == '\n') {
            _col = 0;
            _row++;
        }
        else if (c == '\r') {
            _col = 0;
        }
        else {
            vga_cell(_row, _col)[0] = c;
            vga_cell(_row, _col)[1] = VGA_ATTR;
            if (++_col >= VGA_COLS) {
                _col = 0;
                _row++;
            }
        }
        if (_row >= VGA_ROWS) {
            vga_scroll();
            _row = VGA_ROWS - 1;
        }
    }
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
    for (uint32_t yy = 0; yy < h; ++yy)
        for (uint32_t xx = 0; xx < w; ++xx)
            fb_px(x + xx, y + yy, c);
}

static void fb_fill(void) {
    for (uint32_t y = 0; y < _fb_h; ++y)
        for (uint32_t x = 0; x < _fb_w; ++x)
            fb_px(x, y, FB_BG);
}

static void fb_glyph(uint32_t col, uint32_t row, char c, uint32_t color) {
    if (c < 0x20 || c > 0x7e) {
        c = '.';
    }
    const uint8_t *g = fb_font[c - 0x20];
    uint32_t x0 = col * 8 * _fb_scale;
    uint32_t y0 = row * 16 * _fb_scale;
    for (uint32_t ry = 0; ry < 16; ++ry) {
        uint8_t bits = g[ry];
        for (uint32_t rx = 0; rx < 8; ++rx) {
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
    uint32_t x = _fb_cx * 8 * _fb_scale;
    uint32_t y = (_fb_cy * 16 + 14) * _fb_scale;
    fb_rect(x, y, 8 * _fb_scale, 2 * _fb_scale, c);
}

static void fb_scroll(void) {
    uint32_t cell_bytes = 16 * _fb_scale * _fb_pitch;
    volatile uint8_t *dst = _fb;
    volatile uint8_t *src = _fb + cell_bytes;
    for (uint32_t i = 0; i < cell_bytes * (_fb_rows - 1); ++i) {
        dst[i] = src[i];
    }
    fb_rect(0, (_fb_rows - 1) * 16 * _fb_scale, _fb_w, 16 * _fb_scale, FB_BG);
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

/* 内核 fb 控制台最后一屏的续写位置: 自底向上找第一个非背景行 */
static void fb_locate(void) {
    _fb_cx = 0;
    _fb_cy = 0;
    for (int32_t r = (int32_t)_fb_rows - 1; r >= 0; --r) {
        uint32_t y0 = (uint32_t)r * 16 * _fb_scale;
        uint32_t n = 0;
        for (uint32_t y = y0; y < y0 + 16 * _fb_scale; ++y) {
            for (uint32_t x = 0; x < _fb_w; ++x) {
                volatile uint8_t *p = _fb + (uint64_t)y * _fb_pitch + x * (_fb_bpp / 8);
                uint32_t c = p[0] | (p[1] << 8) | (p[2] << 16);
                if (c != (FB_BG & 0xffffff)) {
                    n++;
                }
            }
        }
        if (n > 0) {
            _fb_cy = (uint32_t)(r + 1);
            if (_fb_cy >= _fb_rows) {
                _fb_cy = _fb_rows - 1;
            }
            return;
        }
    }
}

static int fb_init(void) {
    sys_info_t sysinfo;
    if (syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo) != 0) {
        return -1;
    }
    const x86_platform_data_t *pd = x86_platform_data_of(sysinfo);
    if (pd->fb.phy_base == 0 || pd->fb.pitch == 0 ||
            pd->fb.height == 0 || pd->fb.width == 0 ||
            pd->fb.bpp < 16) {
        return -1;                           /* 无 GOP (BIOS 机器) */
    }
    _fb_w = pd->fb.width;
    _fb_h = pd->fb.height;
    _fb_pitch = pd->fb.pitch;
    _fb_bpp = pd->fb.bpp;
    _fb_scale = (_fb_w >= 1600) ? 2 : 1;
    _fb_cols = _fb_w / (8 * _fb_scale);
    _fb_rows = _fb_h / (16 * _fb_scale);
    if (_fb_cols == 0 || _fb_rows == 0) {
        return -1;
    }
    void *va = (void *)0x40000000UL;
    uint32_t size = _fb_pitch * _fb_h;
    if (syscall3(SYS_MEM_MAP, (ewokos_addr_t)va,
            (ewokos_addr_t)pd->fb.phy_base, (ewokos_addr_t)size) != 0) {
        return -1;
    }
    _fb = (volatile uint8_t *)va;
    fb_locate();
    return 0;
}

static void fb_write(const char* s, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        fb_putchar(s[i]);
    }
}

/* ---- 设备 ---- */
static int vga0_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
        const void* buf, int size, off_t offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)info;
    (void)offset;
    (void)p;
    if (size <= 0) {
        return 0;
    }
    if (_fb != NULL) {
        fb_write((const char*)buf, (uint32_t)size);
    } else if (_vga != NULL) {
        vga_write((const char*)buf, (uint32_t)size);
    }
    return size;
}

static int _keyb_fd = -1;               /* /dev/keyb0: USB/PS2 键盘 (ASCII 流) */

static int vga0_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
        void* buf, int size, off_t offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)info;
    (void)offset;
    (void)p;
    /* 输入侧: 转发 /dev/keyb0 的 ASCII 键入 (hid_keybd getKeyChar 映射) */
    if (_keyb_fd >= 0) {
        int n = read(_keyb_fd, buf, (size_t)size);
        if (n > 0) {
            return n;
        }
    }
    return VFS_ERR_RETRY;
}

int main(int argc, char** argv) {
    const char* mnt_point = argc > 1 ? argv[1] : "/dev/vga0";

    _keyb_fd = open("/dev/keyb0", O_RDONLY);

    if (fb_init() == 0) {
        /* GOP: 内核 fb 控制台最后一屏仍在, 续写位置由 fb_locate 扫描 */
        fb_write("\n--- vgacond: GOP console attached ---\n",
                strlen("\n--- vgacond: GOP console attached ---\n"));
    } else {
        _vga = (volatile char*)VGA_TEXT_VADDR;
        vga_locate();
        vga_write("\n--- vgacond: userspace console attached ---\n",
                strlen("\n--- vgacond: userspace console attached ---\n"));
    }

    vdevice_t dev = {0};
    strcpy(dev.desc, "vgacon");
    dev.read = vga0_read;
    dev.write = vga0_write;
    return device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);
}
