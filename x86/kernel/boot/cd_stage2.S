/* BIOS CD 引导 stage-2 (ISOLINUX 同款 no-emulation 流程)。
 * 链接/装载于 0x0000:0x2000 (线性 0x2000); DS=0 (数据 VA = 线性)。
 * PM 远跳转目标 = 链接 VA = 线性 (load = link ✓)。
 * RM<->PM 乒乓: RM 读 32KB 到 LOW_BUF (0x8000), PM 搬到高地址。
 */
.intel_syntax noprefix
.code16

.include "bootcfg.inc"

.equ KERNEL_LOAD_HIGH, 0x00100000
.equ ROOTFS_LOAD,      0x18000000
.equ BOOTINFO,         0x03000
.equ IO_BUF,           0x04000
.equ LOW_BUF,          0x08000
.equ CD_SECTOR,        2048
.equ CHUNK_SECTORS,    16          /* 16 x 2048B = 32KB/次 */

_start:
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x2f00
    mov [cd_drive], dl

    /* ---- ISO9660: PVD @LBA16 -> IO_BUF ---- */
    mov dword ptr [dap_count], 1
    mov word ptr [dap_buf], IO_BUF
    mov word ptr [dap_buf+2], 0x1000
    mov dword ptr [dap_lba], 16
    mov dword ptr [dap_lba+4], 0
    call read_cd
    mov eax, [IO_BUF+1]
    cmp eax, 0x30314443             /* "CD01" */
    je 1f
    jmp lf_fail
1:
    mov eax, [IO_BUF+156+2]
    mov [dir_extent], eax
    mov eax, [IO_BUF+156+10]
    mov [dir_size], eax

    mov eax, [dir_size]
    add eax, CD_SECTOR-1
    shr eax, 11
    cmp eax, 8
    jbe 2f
    mov eax, 8
2:  mov [dap_count], ax
    mov eax, [dir_extent]
    mov [dap_lba], eax
    mov word ptr [dap_buf], IO_BUF
    mov word ptr [dap_buf+2], 0x1000
    call read_cd

    /* ---- 定位 KERNEL.IMG;1 / ROOTFS.IMG;1 ---- */
    mov dword ptr [name_ptr], offset name_kern
    call find_file
    or eax, eax
    jz lf_fail
    mov [k_extent], eax
    mov [k_size], edx
    mov dword ptr [name_ptr], offset name_root
    call find_file
    or eax, eax
    jz lf_fail
    mov [r_extent], eax
    mov [r_size], edx

    /* ---- bootinfo v3 @BOOTINFO ---- */
    mov di, BOOTINFO
    xor ax, ax
    mov cx, 0x34
3:  stosw
    loop 3b
    mov dword ptr [BOOTINFO+0x00], 0x4B4F5745    /* "EWOK" */
    mov dword ptr [BOOTINFO+0x04], 3
    mov dword ptr [BOOTINFO+0x38], KERNEL_LOAD_HIGH
    mov dword ptr [BOOTINFO+0x58], ROOTFS_LOAD
    mov eax, [r_size]
    mov [BOOTINFO+0x60], eax

    /* ---- KERNEL.IMG ---- */
    mov eax, [k_extent]
    mov [file_lba], eax
    mov eax, [k_size]
    mov [file_size], eax
    mov dword ptr [pm_dst], KERNEL_LOAD_HIGH
    call load_pm

    /* ---- ROOTFS.IMG ---- */
    mov eax, [r_extent]
    mov [file_lba], eax
    mov eax, [r_size]
    mov [file_size], eax
    mov dword ptr [pm_dst], ROOTFS_LOAD
    call load_pm

    /* ---- PM: EDI = bootinfo, 跳内核 ---- */
    in al, 0x92
    or al, 0x02
    out 0x92, al
    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_final

.code32
pm_copy:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x0009f000
    cld
    mov esi, 0x18000
    mov edi, [pm_dst]
    mov ecx, CHUNK_SECTORS*512
    rep movsd
    mov eax, cr0
    and eax, 0x7ffffffe
    mov cr0, eax
    jmp 0x00:rm_back

pm_final:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x0009f000
    mov edi, 0x13000
    jmp dword ptr KERNEL_LOAD_HIGH + KERNEL_ENTRY_OFFSET

.code16
read_cd:
    mov si, offset dap
    mov ah, 0x42
    mov dl, byte ptr [cd_drive]
    int 0x13
    jnc 1f
    jmp lf_fail
1:  ret

find_file:
    mov esi, IO_BUF
    mov ecx, 16384
ff_next:
    movzx eax, byte ptr [esi]
    test eax, eax
    jz ff_none
    cmp byte ptr [esi+25], 0
    jne ff_adv
    cmp byte ptr [esi+32], 12
    jne ff_adv
    push esi
    push edi
    push ecx
    mov edi, esi
    add edi, 33
    mov esi, [name_ptr]
    mov ecx, 12
    repe cmpsb
    pop ecx
    pop edi
    pop esi
    jne ff_adv
    mov eax, [esi+2]
    mov edx, [esi+10]
    ret
ff_adv:
    movzx eax, byte ptr [esi]
    sub ecx, eax
    jc ff_none
    add esi, eax
    jmp ff_next
ff_none:
    xor eax, eax
    xor edx, edx
    ret

lf_fail:
    mov si, offset msg_img
    jmp prt_fail
fail_cd:
    mov si, offset msg_cd
prt_fail:
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

msg_cd:
    .asciz "E:CD"
msg_img:
    .asciz "E:I"

.align 4
cd_drive:
    .byte 0
name_ptr:
    .long 0
dir_extent:
    .long 0
dir_size:
    .long 0
k_extent:
    .long 0
k_size:
    .long 0
r_extent:
    .long 0
r_size:
    .long 0
r_sectors:
    .long 0
file_lba:
    .long 0
file_size:
    .long 0
pm_dst:
    .long 0
dap:
    .byte 0x10
    .byte 0
dap_count:
    .word 1
dap_buf:
    .long 0
dap_lba:
    .quad 0
name_kern:
    .ascii "KERNEL.IMG;1"
name_root:
    .ascii "ROOTFS.IMG;1"

.align 4
gdt:
    .quad 0
    .quad 0x00cf9a000000ffff
    .quad 0x00cf92000000ffff
gdt_ptr:
    .word (3*8)-1
    .long gdt
