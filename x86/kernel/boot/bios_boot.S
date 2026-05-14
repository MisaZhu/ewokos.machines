.intel_syntax noprefix
.code16

.include "bootcfg.inc"

.equ KERNEL_LOAD_LOW,      0x00010000
.equ KERNEL_LOAD_LOW_SEG,  0x1000
.equ KERNEL_LOAD_HIGH,     0x00100000
.equ TRACK_SECTORS,        18
.equ TRACK_HEADS,          2
.equ TRACK_SIZE,           (TRACK_SECTORS * TRACK_HEADS)
.equ KERNEL_COPY_DWORDS,   (KERNEL_SECTORS * 128)
.equ COM1_PORT,            0x3F8

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
    mov word ptr [current_lba], 1
    mov word ptr [load_segment], KERNEL_LOAD_LOW_SEG
    call serial_init
    mov al, 'A'
    call serial_putc

load_kernel:
    mov ax, word ptr [current_lba]
    cmp ax, KERNEL_SECTORS + 1
    jae enter_protected_mode

    xor dx, dx
    mov cx, TRACK_SIZE
    div cx
    mov bl, al

    mov ax, dx
    xor dx, dx
    mov cx, TRACK_SECTORS
    div cx
    mov ch, bl
    mov dh, al
    mov cl, dl
    inc cl

    mov ax, word ptr [load_segment]
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, 0x01
    mov dl, byte ptr [boot_drive]
    int 0x13
    jc disk_error

load_kernel_done:
    add word ptr [load_segment], 0x20
    inc word ptr [current_lba]
    jmp load_kernel

enter_protected_mode:
    mov al, 'B'
    call serial_putc
    in al, 0x92
    or al, 0x02
    out 0x92, al

    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 0x00000001
    mov cr0, eax
    jmp 0x08:protected_mode

disk_error:
    mov al, 'E'
    call serial_putc
    cli
hang:
    hlt
    jmp hang

serial_init:
    mov dx, COM1_PORT + 1
    xor al, al
    out dx, al
    mov dx, COM1_PORT + 3
    mov al, 0x80
    out dx, al
    mov dx, COM1_PORT + 0
    mov al, 0x01
    out dx, al
    mov dx, COM1_PORT + 1
    xor al, al
    out dx, al
    mov dx, COM1_PORT + 3
    mov al, 0x03
    out dx, al
    mov dx, COM1_PORT + 2
    mov al, 0xC7
    out dx, al
    mov dx, COM1_PORT + 4
    mov al, 0x0B
    out dx, al
    ret

serial_putc:
    push ax
1:
    mov dx, COM1_PORT + 5
    in al, dx
    test al, 0x20
    jz 1b
    pop ax
    mov dx, COM1_PORT
    out dx, al
    ret

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
current_lba:
    .word 0
load_segment:
    .word 0

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

    cmp dword ptr [KERNEL_LOAD_LOW + KERNEL_ENTRY_OFFSET], KERNEL_ENTRY_MAGIC
    jne source_error
    cmp dword ptr [KERNEL_LOAD_HIGH + KERNEL_ENTRY_OFFSET], KERNEL_ENTRY_MAGIC
    jne copy_error

    mov dx, COM1_PORT
    mov al, 'C'
    out dx, al
    mov eax, KERNEL_LOAD_HIGH + KERNEL_ENTRY_OFFSET
    jmp eax

source_error:
    mov dx, COM1_PORT
    mov al, 'S'
    out dx, al
    mov al, byte ptr [KERNEL_LOAD_LOW + 0x1000]
    call serial32_hex8
    mov al, byte ptr [KERNEL_LOAD_LOW + 0x2000]
    call serial32_hex8
    mov al, byte ptr [KERNEL_LOAD_LOW + 0x3000]
    call serial32_hex8
    mov al, byte ptr [KERNEL_LOAD_LOW + 0x4000]
    call serial32_hex8
    mov al, byte ptr [KERNEL_LOAD_LOW + 0x5000]
    call serial32_hex8
    mov al, byte ptr [KERNEL_LOAD_LOW + 0x5E00]
    call serial32_hex8
    jmp hang32

copy_error:
    mov dx, COM1_PORT
    mov al, 'M'
    out dx, al
    jmp hang32

hang32:
    cli
1:
    hlt
    jmp 1b

serial32_wait:
    mov dx, COM1_PORT + 5
1:
    in al, dx
    test al, 0x20
    jz 1b
    ret

serial32_putc:
    push eax
    push edx
    call serial32_wait
    pop edx
    pop eax
    mov dx, COM1_PORT
    out dx, al
    ret

serial32_hex8:
    push eax
    mov ah, al
    shr al, 4
    call serial32_hex_nibble
    mov al, ah
    and al, 0x0f
    call serial32_hex_nibble
    pop eax
    ret

serial32_hex_nibble:
    and al, 0x0f
    cmp al, 10
    jb 1f
    add al, 'A' - 10
    jmp 2f
1:
    add al, '0'
2:
    jmp serial32_putc

.org 510
.word 0xaa55
