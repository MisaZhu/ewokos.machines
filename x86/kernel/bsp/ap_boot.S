.intel_syntax noprefix

.equ X86_AP_TRAMPOLINE_PADDR, 0x7000
.equ IA32_EFER_MSR, 0xC0000080
.equ CR0_PE, 0x00000001
.equ CR0_PG, 0x80000000
.equ CR4_PAE, 0x00000020

.section .text
.code16

.global x86_ap_trampoline_start
.global x86_ap_trampoline_end
.global x86_ap_trampoline_boot_pml4
.global x86_ap_trampoline_kernel_vm
.global x86_ap_trampoline_stack
.global x86_ap_trampoline_kernel_entry
.global x86_ap_trampoline_core_id
.global x86_ap_entry64
.extern _slave_kernel_entry_c

x86_ap_trampoline_start:
    cli
    cld
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x0ff0

    xor eax, eax
    mov ax, cs
    shl eax, 4
    add eax, x86_ap_gdt - x86_ap_trampoline_start
    mov dword ptr [x86_ap_gdt_ptr - x86_ap_trampoline_start + 2], eax
    lgdt [x86_ap_gdt_ptr - x86_ap_trampoline_start]

    mov eax, cr0
    or eax, CR0_PE
    mov cr0, eax
    jmp 0x08:(X86_AP_TRAMPOLINE_PADDR + x86_ap_pm_entry - x86_ap_trampoline_start)

.code32
x86_ap_pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, X86_AP_TRAMPOLINE_PADDR + 0x0ff0

    call x86_ap_pm_base
x86_ap_pm_base:
    pop ebx
    sub ebx, x86_ap_pm_base - x86_ap_trampoline_start

    mov eax, cr4
    or eax, CR4_PAE
    mov cr4, eax

    mov eax, dword ptr [ebx + x86_ap_trampoline_boot_pml4 - x86_ap_trampoline_start]
    mov cr3, eax

    mov ecx, IA32_EFER_MSR
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, CR0_PG
    mov cr0, eax
    jmp 0x18:(X86_AP_TRAMPOLINE_PADDR + x86_ap_lm_entry - x86_ap_trampoline_start)

.code64
x86_ap_lm_entry:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov rsp, X86_AP_TRAMPOLINE_PADDR + 0x0ff0
    movabs rax, X86_AP_TRAMPOLINE_PADDR + x86_ap_entry64 - x86_ap_trampoline_start
    jmp rax

.align 16
x86_ap_gdt:
    .quad 0x0000000000000000
    .quad 0x00cf9a000000ffff
    .quad 0x00cf92000000ffff
    .quad 0x00af9a000000ffff
    .quad 0x00cf92000000ffff
x86_ap_gdt_ptr:
    .word (5 * 8) - 1
    .long 0

.align 16
x86_ap_trampoline_boot_pml4:
    .quad 0
x86_ap_trampoline_kernel_vm:
    .quad 0
x86_ap_trampoline_stack:
    .quad 0
x86_ap_trampoline_kernel_entry:
    .quad 0
x86_ap_trampoline_core_id:
    .long 0
    .long 0

.code64
x86_ap_entry64:
    mov rcx, qword ptr [X86_AP_TRAMPOLINE_PADDR + x86_ap_trampoline_stack - x86_ap_trampoline_start]
    mov rsp, rcx
    xor ebp, ebp
    mov rax, qword ptr [X86_AP_TRAMPOLINE_PADDR + x86_ap_trampoline_kernel_entry - x86_ap_trampoline_start]
    mov edi, dword ptr [X86_AP_TRAMPOLINE_PADDR + x86_ap_trampoline_core_id - x86_ap_trampoline_start]
    jmp rax

x86_ap_trampoline_end:
