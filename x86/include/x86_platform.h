/*
 * x86_platform.h — x86 平台 sys_info.platform_data 布局定义。
 *
 * 内核侧 (machines/x86/kernel/bsp) 与用户态驱动 (bsp_fb/bsp_sd/vgacond)
 * 共用本头文件解析 sys_info_t.platform_data 字节区; 内核侧另提供
 * 全局实例 x86_platform_data (定义于 bsp/hw_info_arch.c)。
 */
#ifndef X86_PLATFORM_H
#define X86_PLATFORM_H

#include <stdint.h>
#include <sysinfo.h>

/* pre-configured linear framebuffer handed over by the bootloader
   (UEFI GOP); phy_base == 0 means no bootloader framebuffer */
typedef struct {
	uint64_t phy_base;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t bpp;
} x86_fb_info_t;

typedef struct {
	x86_fb_info_t fb;
	/* 内存盘 rootfs (x86 UEFI 引导时 stub 拷入 RAM 的 ROOTFS.IMG);
	   phy_base == 0 表示未提供 */
	dma_info_t    rd;
} x86_platform_data_t;

/* 用户态: 从 SYS_GET_SYS_INFO 拷贝出的 sys_info_t 解析平台数据 */
#define x86_platform_data_of(si) ((const x86_platform_data_t *)(si).platform_data)

/* 内核侧全局实例 (定义于 machines/x86/kernel/bsp/hw_info_arch.c);
 * 用户态不引用 (经上方宏解析自己的 sysinfo 拷贝) */
extern x86_platform_data_t x86_platform_data;

#endif
