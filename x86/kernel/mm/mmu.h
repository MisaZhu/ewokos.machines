/*
 * mm/mmu.h — x86 平台 overlay (machines/x86/kernel 位于内核构建 -I 首位,
 * 本文件先于共享 kernel/kernel/include/mm/mmu.h 命中)。
 *
 * 只承载 x86 内核的宏级修正; 共享定义经 #include_next 引入后按需覆盖。
 * 用户态构建不经过本目录 —— 覆盖不得改变内核/用户态共享的 ABI 布局。
 */
#ifndef X86_MMU_OVERLAY_H
#define X86_MMU_OVERLAY_H

#include_next <mm/mmu.h>

/* 页对齐: E820/GOP 上报的内存总量非规整值时 (如 255MB), 未对齐尾数会让
 * KMALLOC_BASE 落在页中间, kmalloc 初始化即 Panic (GPD 实测)。
 * 共享公式对所有平台本也应如此, 暂以 overlay 收敛为 x86 专属 */
#undef ALLOCABLE_PAGE_DIR_SIZE
#define ALLOCABLE_PAGE_DIR_SIZE       (ALIGN_UP(2 * (_sys_info.total_phy_mem_size / KB), PAGE_SIZE))

#endif
