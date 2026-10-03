/*
 * cd_stage1.S — El Torito no-emulation BIOS-CD 引导 (Linux live ISO 同款)。
 *
 * 常规路径: BIOS 按目录 SectorCount 一次性装载 loader(2KB) + 内核(~210KB)
 * 到 0x0000:0x7C00, loader 在 PM 下把内核从线性 0x8400 搬到 0x100000。
 * 真机加固 (SeaBIOS 全量装载是理想情形, 真机 BIOS 对 no-emulation 装载量
 * 实现不一, ISOLINUX 同样只信任 2KB):
 *   1. 装载后校验内核入口魔数, 缺失 (BIOS 截断装载) 则走 INT13h CD 扩展读
 *      回退装载内核 (DL = El Torito 传来的 CD 盘号, 2KB LBA 由 mkboot 补丁);
 *   2. INT15h E820 捕获到 0000:5000 (bsp/hw_info_arch.c 消费) —— 内核
 *      否则只能假设 512MB, 小内存真机会越界;
 *   3. PM 搬运后再次校验魔数 (A20 失效等造成的低 1MB 环绕可在此拦截)。
 * 全程不依赖 INT13h 也能工作 (固件不支持扩展读且全量装载成功时)。
 *
 * 链接于 0x7C00 (装载线性地址, 与 bios_boot.S 同一手法):
 * DS=0 时 绝对地址 == 线性地址, 运行期无需任何地址修正。
 * x86/x64: 实模式 → PM → 内核长模式。
 */
.intel_syntax noprefix
.code16

.include "bootcfg.inc"

.equ KERNEL_LOAD_HIGH, 0x00100000
.equ KERN_OFF,         0x800                 /* loader 2KB 之后的内核偏移 */
.equ E820_BUF,         0x5000                /* 与 bios_boot.S 原捕获块同址 */
.equ E820_TMP,         0x4C00                /* INT15h 单条目临时缓冲 (BIOS 不推进 DI) */
.equ CD_CHUNK,         16                    /* 回退读: 16 x 2KB = 32KB/次 */
.equ CD_REMAIN,        (KERNEL_SECTORS + 3) / 4   /* 内核 2KB 扇区数 */

.global _start

_start:
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    mov [cd_drive], dl

    /* 真机定位标记: VGA 屏幕底行 (24,40) 起, 内核首条日志清屏覆盖。
     * '1'=stage1 入口 '8'=E820 完成 'R'=INT13h 回退 '.'=每 32KB 块 */
    mov ax, 0xB800
    mov gs, ax
    mov byte ptr [gs:0xF50], '1'

    /* A20 */
    in al, 0x92
    or al, 0x02
    out 0x92, al

    /* INT15h E820 捕获 (0x5000: magic u32 + count u32 + 24B/项):
     * 项 = base(8) + len(8) + type(4) + pad(4), 与 hw_info_arch.c 对齐。
     * BIOS 不保证推进 DI (SeaBIOS 每次写同一位置): 先读入临时缓冲,
     * 再手工按 24B 步长转存到 0x5008 起 */
    mov dword ptr [E820_BUF], 0x45383230       /* 'E820' LE */
    xor bp, bp
    xor ebx, ebx
1:
    mov di, E820_TMP
    mov eax, 0xe820
    mov edx, 0x534d4150                        /* SMAP 签名 (每次调用都要) */
    mov ecx, 20
    int 0x15
    jc 2f
    cmp eax, 0x534d4150                        /* SMAP */
    jne 2f
    mov ax, bp                                 /* 输出槽 = 0x5008 + bp*24 */
    mov cx, 24
    mul cx
    add ax, E820_BUF + 8
    mov di, ax
    mov si, E820_TMP
    mov cx, 10
    rep movsw                                  /* 20B 转存 */
    mov dword ptr [di], 0                      /* pad 4B 清零 */
    inc bp
    cmp bp, 128
    jae 2f
    test ebx, ebx
    jnz 1b
2:
    mov dword ptr [E820_BUF + 4], ebp          /* count */
    mov byte ptr [gs:0xF52], '8'

    /* BIOS 预装载的内核入口魔数校验: 失败 (真机 BIOS 截断 no-emulation
     * 装载) 走 INT13h CD 回退装载 */
    cmp dword ptr [0x8400 + KERNEL_ENTRY_OFFSET], KERNEL_ENTRY_MAGIC
    je pm_enter
    call cd_load_kernel

pm_enter:
    /* GDT + 保护模式 (绝对地址 = 线性地址) */
    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
pm_jmp:
    .byte 0xEA                           /* far jmp 0x0008:pm_start (线性) */
    .word pm_start
    .word 0x08                           /* GDT: null@0x00, code32@0x08, data32@0x10 */

fail:
    mov si, offset msg_err
    push 0xB800
    pop es
    xor di, di
1:  lodsb
    or al, al
    jz 2f
    stosb
    inc di
    jmp 1b
2:
    cli
    hlt
    jmp 2b

msg_err:
    .asciz "E:CD"

/* ---- INT13h CD 回退装载: 整份内核从补丁的 2KB LBA 起读到 0x8400。
 * CD 的 INT13h 扩展读以 2KB 扇区为 LBA 单位 (ATAPI 原生), DAP count
 * 同为 2KB 单位。全程实模式, 32KB/次, 段跨越经 AX 手工递增 ES。 ---- */
cd_load_kernel:
    /* AH=41h 扩展读探测 (DL = El Torito CD 盘号); 失败则无计可施 */
    mov ah, 0x41
    mov bx, 0x55aa
    mov dl, [cd_drive]
    int 0x13
    jc fail
    cmp bx, 0xaa55
    jne fail
    mov byte ptr [gs:0xF54], 'R'

    mov bp, CD_REMAIN                    /* 剩余 2KB 扇区 */
    mov eax, [kcdl_slot]                 /* mkboot 补丁的内核 2KB LBA */
    mov [cd_lba], eax
    mov ax, 0x0840                       /* ES:BX = 线性 0x8400 起 */
    mov es, ax
    xor bx, bx
cdl_loop:
    test bp, bp
    jz cdl_done
    mov ax, CD_CHUNK
    cmp ax, bp
    jbe 1f
    mov ax, bp                           /* 尾块 */
1:
    mov [dap_count], ax
    sub bp, ax
    mov [dap_off], bx
    mov [dap_seg], es
    mov eax, [cd_lba]
    mov [dap_lba], eax
    mov si, offset dap
    mov ah, 0x42
    mov dl, [cd_drive]
    int 0x13
    jc fail
    mov ax, es
    add ax, CD_CHUNK*128                  /* ES 前进 32KB = 0x800 段落 */
    mov es, ax
    add dword ptr [cd_lba], CD_CHUNK
    mov byte ptr [gs:0xF56], '.'              /* 每块进度点 */
    jmp cdl_loop
cdl_done:
    ret

.align 4
cd_drive:
    .byte 0
kcdl_slot:
    .long 0x4B444331                     /* "1CDK" 魔数槽: mkboot 补丁内核 2KB LBA */
cd_lba:
    .long 0
.align 4
dap:
    .byte 0x10
    .byte 0
dap_count:
    .word 0
dap_off:
    .word 0
dap_seg:
    .word 0
dap_lba:
    .long 0
    .long 0

.align 4
gdt:
    .quad 0
    .quad 0x00cf9a000000ffff        /* code32: base=0, limit=4GB */
    .quad 0x00cf92000000ffff        /* data32: base=0, limit=4GB */
gdt_ptr:
    .word (3*8)-1
    .long gdt                       /* 链接偏移, 运行时 +0x7c00 = 线性 */

/* ---- 32 位保护模式 (flat segments): 搬内核, 校验, 跳内核 ---- */
.code32
pm_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x00090000

    /* 内核: 线性 0x7C00+KERN_OFF = 0x8400 -> 0x100000 */
    mov esi, 0x7c00 + KERN_OFF
    mov edi, KERNEL_LOAD_HIGH
    mov ecx, 0x40000/4              /* 256KB 覆盖 */
    rep movsd

    /* 搬运后魔数校验: 拦截 A20 失效 (低 1MB 环绕) 与残余截断 */
    cmp dword ptr [KERNEL_LOAD_HIGH + KERNEL_ENTRY_OFFSET], KERNEL_ENTRY_MAGIC
    jne fail_pm

    /* 跳内核 (rdi=0: BIOS-CD 无内存盘路径, hw_info 魔数兜底)。
     * 用寄存器间接直跳: `jmp dword ptr <绝对地址>` 会被 gas 编码成
     * 读内存的间接跳转 (ff 25), 而 0x10702a 处是入口魔数不是地址 */
    xor edi, edi
    mov eax, KERNEL_LOAD_HIGH + KERNEL_ENTRY_OFFSET
    jmp eax

fail_pm:
    /* VGA 报错 "E:I" (镜像坏/A20 失效), 与 bios_boot.S 语义一致 */
    mov esi, 0xB8000
    mov byte ptr [esi], 'E'
    mov byte ptr [esi+2], ':'
    mov byte ptr [esi+4], 'I'
1:
    cli
    hlt
    jmp 1b
