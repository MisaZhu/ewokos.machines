/*
 * Mock payload for the QEMU UEFI smoke test.
 *
 * Linked at 0x80080000 (Q8B_QEMU_TEST build of BOOTAA64.EFI).  It writes
 * a banner and the bootinfo magic/framebuffer fields to the QEMU virt
 * PL011 at 0x09000000, proving that the loader loaded the file, filled the
 * bootinfo page, exited boot services and jumped with the MMU still on.
 */
    .section .text
    .global _start
_start:
    ldr x1, =0x09000000          /* QEMU virt PL011 data register */
    adr x2, banner
1:  ldrb w3, [x2], #1
    cbz w3, 2f
    str w3, [x1]
    b 1b
2:
    /* bootinfo @ 0x80070000: magic, version, fb_addr, rd_base, rd_size */
    ldr x4, =0x80070000
    ldr w5, [x4, #0]             /* magic */
    bl put_hex
    mov w3, #13
    str w3, [x1]
    mov w3, #10
    str w3, [x1]

    ldr x5, [x4, #16]            /* fb_addr */
    bl put_hex64
    mov w3, #13
    str w3, [x1]
    mov w3, #10
    str w3, [x1]

    ldr x5, [x4, #40]            /* rd_base */
    bl put_hex64
    mov w3, #13
    str w3, [x1]
    mov w3, #10
    str w3, [x1]

3:  wfi
    b 3b

/* print w5 as 8 hex digits */
put_hex:
    mov x6, #8
    mov w7, #28
4:  lsr w9, w5, w7
    and w9, w9, #0xf
    cmp w9, #9
    b.hi 5f
    add w9, w9, #'0'
    b 6f
5:  add w9, w9, #('a' - 10)
6:  str w9, [x1]
    sub w7, w7, #4
    subs x6, x6, #1
    b.ne 4b
    ret

/* print x5 as 16 hex digits */
put_hex64:
    mov x6, #16
    mov w7, #60
7:  lsr x9, x5, x7
    and w9, w9, #0xf
    cmp w9, #9
    b.hi 8f
    add w9, w9, #'0'
    b 9f
8:  add w9, w9, #('a' - 10)
9:  str w9, [x1]
    sub w7, w7, #4
    subs x6, x6, #1
    b.ne 7b
    ret

banner:
    .asciz "\r\nMOCK KERNEL OK: bootinfo magic/fb/rd = "
    .align 3
