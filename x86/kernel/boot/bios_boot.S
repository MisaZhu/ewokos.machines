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

load_kernel:
    cmp word ptr [remaining_sectors], 0
    je enter_protected_mode

    lea si, [dap]
    mov ah, 0x42
    mov dl, byte ptr [boot_drive]
    int 0x13
    jc disk_error

    add word ptr [dap_segment], 0x20
    inc dword ptr [dap_lba_low]
    dec word ptr [remaining_sectors]
    jmp load_kernel

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
hang16:
    cli
1:
    hlt
    jmp 1b

.align 8
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
dap:
    .byte 0x10
    .byte 0x00
    .word 0x0001
    .word 0x0000
dap_segment:
    .word KERNEL_LOAD_LOW_SEG
dap_lba_low:
    .long 1
    .long 0

.code32
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x00090000
    cld

    mov esi, KERNEL_LOAD_LOW
    mov edi, KERNEL_LOAD_HIGH
    mov ecx, KERNEL_COPY_DWORDS
    rep movsd

    cmp dword ptr [KERNEL_LOAD_HIGH + KERNEL_ENTRY_OFFSET], KERNEL_ENTRY_MAGIC
    jne hang32
    mov eax, KERNEL_LOAD_HIGH + KERNEL_ENTRY_OFFSET
    jmp eax

hang32:
    cli
2:
    hlt
    jmp 2b

.org 446
    .fill 64, 1, 0
.word 0xaa55
