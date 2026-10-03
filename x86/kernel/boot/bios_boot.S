.intel_syntax noprefix
.code16

.include "bootcfg.inc"

.equ KERNEL_LOAD_LOW,      0x00010000
.equ KERNEL_LOAD_LOW_SEG,  0x1000
.equ KERNEL_LOAD_HIGH,     0x00100000
.equ KERNEL_COPY_DWORDS,   (KERNEL_SECTORS * 128)

.section .text
.global _start

_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    mov byte ptr [boot_drive], dl
    /* 内核起始 LBA: dap_lba_low 初始值即 mkboot 补丁槽 ("EWOK" 魔数,
     * hdd/floppy=1; ISO 的完整内核拷贝在文件区 KERNEL.IMG —— system
     * area 字节 512 起的 isohybrid 拷贝被 PVD@LBA16 截断, 不可引导)。
     * 压栈存底, CHS 回退整卷重读时恢复 */
    push dword ptr [dap_lba_low]

    /* chainload 环境 (GRUB 等) 交接时可能改动了 8259 的向量基址/掩码,
     * SeaBIOS 的 INT13h 传输路径等 IRQ14 会被永久挂起: 重新初始化两片
     * PIC 到标准映射 (master 0x08 / slave 0x70) 并放开全部 IRQ */
    mov al, 0x11
    out 0x20, al
    out 0xA0, al
    mov al, 0x08
    out 0x21, al
    mov al, 0x70
    out 0xA1, al
    mov al, 0x04
    out 0x21, al
    mov al, 0x02
    out 0xA1, al
    mov al, 0x01
    out 0x21, al
    out 0xA1, al
    xor al, al
    out 0x21, al
    out 0xA1, al

    /* INT13h 扩展读探测 (AH=41h): El Torito 仿真盘只支持 AH<=16h 的
     * 老式命令 (SeaBIOS 对仿真盘的扩展命令一律返回 EPARAM), 探测失败
     * 则回退 CHS 读 (几何经 AH=08 查询, 查不到用 255头/63扇区) */
    mov ah, 0x41
    mov bx, 0x55aa
    mov dl, byte ptr [boot_drive]
    int 0x13
    jc load_kernel_chs
    cmp bx, 0xaa55
    jne load_kernel_chs

load_kernel:
    cmp word ptr [remaining_sectors], 0
    je enter_protected_mode

    lea si, [dap]
    mov ah, 0x42
    mov dl, byte ptr [boot_drive]
    int 0x13
    /* AH=41 谎报支持的固件/钩子 (如部分 chainload 环境) 会在这里失败:
     * 回退 CHS 重读整份内核, 而不是停在 disk_error */
    jc load_kernel_chs

    add word ptr [dap_segment], 0x20
    inc dword ptr [dap_lba_low]
    dec word ptr [remaining_sectors]
    jmp load_kernel

    /* CHS 回退: LBA → CHS, 几何优先 INT13h AH=08 实测 (QEMU 按容量给
     * 16头/63扇区, 硬编码 255 头会整盘读挂), 查询失败用 255/63 */
load_kernel_chs:
    mov word ptr [remaining_sectors], KERNEL_SECTORS
    pop dword ptr [dap_lba_low]              /* 恢复补丁的内核起始 LBA */
    mov ax, 0x1000
    mov es, ax
    xor bx, bx
    /* geom_heads/geom_spt 静态默认 254/63 (=255/63); AH=08 成功则覆盖 */
    mov ah, 0x08
    mov dl, byte ptr [boot_drive]
    push es
    int 0x13
    pop es
    jc 1f
    test cl, 0x3f
    jz 1f
    mov [geom_heads], dh                    /* DH = 最大头号 */
    mov al, cl
    and al, 0x3f
    mov [geom_spt], al                      /* CL[5:0] = 每磁道扇区 */
1:
chs_loop:
    mov eax, [dap_lba_low]
    xor edx, edx
    movzx ecx, byte ptr [geom_heads]
    inc ecx                                 /* 头数 = 最大头号 + 1 */
    movzx esi, byte ptr [geom_spt]
    imul ecx, esi                           /* 每柱面扇区数 */
    div ecx                                 /* eax = 柱面, edx = 余数 */
    mov edi, eax
    mov eax, edx
    xor edx, edx
    div esi                                 /* eax = 磁头, edx = 扇区-1 */
    mov dh, al
    inc edx
    mov cl, dl                              /* 扇区 1..63+ */
    mov ax, di
    mov ch, al                              /* 柱面低 8 位 */
    shr ax, 8
    and ax, 3
    shl ax, 6
    or cl, al                               /* 柱面高 2 位 → CL[7:6] */
    mov dl, byte ptr [boot_drive]
    mov al, 1                               # 每次读 1 个扇区
    mov ah, 0x02
    int 0x13
    jc disk_error
    add bx, 0x200            # 512 字节 (BX 是段内字节偏移)
    jnc 1f
    mov ax, es               # 64KB 段跨越
    add ax, 0x1000
    mov es, ax
1:
    inc dword ptr [dap_lba_low]
    dec word ptr [remaining_sectors]
    jnz chs_loop
    jmp enter_protected_mode

    /* 注: 原 E820 捕获块 (0000:5000) 位于两个跳转目标之间, 从 loop 顶部
     * `je enter_protected_mode` 出口看不可达 (死代码), 已删除以留出
     * 引导扇区空间给 CHS 回退路径; 内核内存探测不依赖 0x5000 魔数。 */

enter_protected_mode:
    in al, 0x92
    or al, 0x02
    out 0x92, al

    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 0x00000001
    mov cr0, eax
    jmp 0x08:protected_mode

disk_error:
    /* VGA 报错 (无串口平台挂死点可见): 行 0 写 "E:D" */
    push 0xB800
    pop es
    mov si, msg_disk
    xor di, di
    jmp prt16_loop
hang16:
    cli
1:
    hlt
    jmp 1b

/* ES:DI = VGA 行 0, DS:SI = NUL 结尾串, 只写字符 (属性沿用 0x07) */
prt16_loop:
    lodsb
    or al, al
    jz 2f
    stosb
    inc di
    jmp 1b
2:  ret

gdt:
    .quad 0x0000000000000000
    .quad 0x00cf9a000000ffff
    .quad 0x00cf92000000ffff
gdt_ptr:
    .word (3 * 8) - 1
    .long gdt

boot_drive:
    .byte 0
remaining_sectors:
    .word KERNEL_SECTORS
geom_heads:
    .byte 254                               /* CHS 回退几何: 默认 255 头 */
geom_spt:
    .byte 63                                /* 默认每磁道 63 扇区 */
dap:
    .byte 0x10
    .byte 0x00
    .word 0x0001
    .word 0x0000
dap_segment:
    .word KERNEL_LOAD_LOW_SEG
dap_lba_low:
    .long 0x4B4F5745                        /* "EWOK" 魔数 = mkboot 补丁槽: 内核起始 LBA */
    .long 0                                 /* LBA 高 32 位 (恒 0, <4GB) */
msg_disk:
    .asciz "E:D"
msg_img:
    .asciz "E:I"

.code32
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x00090000
    cld

    mov esi, KERNEL_LOAD_LOW
    mov edi, KERNEL_LOAD_HIGH
    mov ecx, KERNEL_COPY_DWORDS
    rep movsd

    cmp dword ptr [KERNEL_LOAD_HIGH + KERNEL_ENTRY_OFFSET], KERNEL_ENTRY_MAGIC
    jne hang32

    xor edi, edi                    /* BIOS 硬盘路径无 bootinfo (魔数兜底) */
    mov eax, KERNEL_LOAD_HIGH + KERNEL_ENTRY_OFFSET
    jmp eax

hang32:
    /* VGA 报错: 行 0 写 "E:I" (入口魔数不符, 内核镜像损坏)。
     * PM 入口已 cld, stosb 自增 EDI, 无需显式 inc (引导扇区 446B 预算) */
    mov esi, msg_img
    mov edi, 0xB8000
1:  lodsb
    or al, al
    jz 2f
    stosb
    jmp 1b
2:
    cli
3:
    hlt
    jmp 3b

.org 446
    .fill 64, 1, 0
.word 0xaa55
