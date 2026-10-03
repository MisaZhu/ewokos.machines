/* efi_stub.S — UEFI 入口与 SysV→MS x64 ABI shim
 * (移植自 kernel-testsuite, 已在 QEMU OVMF 上验证)
 *
 * 固件以 MS x64 ABI 调用镜像入口: rcx=ImageHandle, rdx=SystemTable。
 * EFI 函数指针同样按 MS x64 调用:
 *   MS: rcx=a1, rdx=a2, r8=a3, r9=a4, [rsp+32]=a5...; caller 备 32B shadow; call 前 rsp 16 对齐
 * SysV 入参: rdi=fn, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5
 *
 * 两个经典坑:
 *  1) 不能 "mov rcx,fn; call rcx" — rcx 是 MS 第1参寄存器, 固件会把函数指针当第1参!
 *     → 用 rdi 保存并调用函数指针 (rdi 在 MS ABI 中不是参数寄存器)。
 *  2) a2 已在 rdx, 千万别为"腾位置"把它挪去 r8 — MS 的 a2 就是 rdx!
 */
    .intel_syntax noprefix
    .text

/* EFI 入口: 转 SysV 后进入 C */
    .globl _start
_start:
    mov  rdi, rcx                  /* ImageHandle */
    mov  rsi, rdx                  /* SystemTable */
    sub  rsp, 0x28                 /* 对齐 + shadow */
    call efi_main
    add  rsp, 0x28
    ret

    .globl efi_call1
efi_call1:                         /* fn, a1 */
    mov  rcx, rsi                  /* a1 */
    sub  rsp, 40
    call rdi
    add  rsp, 40
    ret

    .globl efi_call2
efi_call2:                         /* fn, a1, a2 */
    mov  rcx, rsi                  /* a1 (a2 已在 rdx) */
    sub  rsp, 40
    call rdi
    add  rsp, 40
    ret

    .globl efi_call3
efi_call3:                         /* fn, a1, a2, a3 */
    mov  r11, rcx                  /* a3 暂存 */
    mov  rcx, rsi                  /* a1 (a2 已在 rdx) */
    mov  r8,  r11                  /* a3 */
    sub  rsp, 40
    call rdi
    add  rsp, 40
    ret

    .globl efi_call4
efi_call4:                         /* fn, a1, a2, a3, a4 */
    push rbp
    mov  rbp, rsp
    mov  r10, r8                   /* a4 暂存 */
    mov  r11, rcx                  /* a3 暂存 */
    mov  rcx, rsi                  /* a1 */
    mov  r8,  r11                  /* a3 */
    mov  r9,  r10                  /* a4 */
    sub  rsp, 48                   /* 8(rbp)+48=56: -56+8 ≡ 0 mod 16, 对齐! */
    call rdi
    mov  rsp, rbp
    pop  rbp
    ret

    .globl efi_call5
efi_call5:                         /* fn, a1, a2, a3, a4, a5 */
    push rbp
    mov  rbp, rsp
    push r8                        /* a4 暂存 → rbp-8 */
    push r9                        /* a5 暂存 → rbp-16 */
    mov  r11, rcx                  /* a3 暂存 */
    mov  rcx, rsi                  /* a1 */
    mov  r8,  r11                  /* a3 */
    mov  r9,  [rbp-8]              /* a4 */
    mov  rax, [rbp-16]             /* a5 */
    sub  rsp, 48                   /* 32B shadow + 8 栈参槽 */
    mov  [rsp+32], rax             /* 第5参上栈 */
    call rdi
    mov  rsp, rbp
    pop  rbp
    ret

    .section .note.GNU-stack,"",@progbits
