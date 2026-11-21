/*
 * Install the kernel's real exception vector table after the boot MMU is on.
 *
 * This must live in .init and use a literal pool (absolute 64-bit address),
 * NOT a C-level &symbol: the C compiler emits a GOT load for the address and
 * the GOT sits more than 4GB away from .init, outside ADRP's reach.
 */
    .section ".bootvec"
    .align 3
    .global orin_install_vectors
    .type orin_install_vectors, %function
orin_install_vectors:
    ldr x0, =interrupt_table_start
    msr vbar_el1, x0
    isb
    ret
    .size orin_install_vectors, . - orin_install_vectors

/*
 * Tegra234 logical core id from MPIDR.  The generic boot.S merge
 * (Aff0 | Aff1) drops Aff2 (the cluster), so Orin Nano cluster1 cores
 * (Aff2=1, Aff1=2/3) merge to the same ids as cluster0 cores 2/3.  The
 * MPIDR constants are encoded as immediates (not loaded from the BSP table)
 * so boot.S can call this at entry with the MMU off, before the high-half
 * is mapped.  Returns the logical id 0..5 in x0; 6 for an unknown MPIDR
 * (treated as a secondary).  Clobbers x0-x2 only.
 */
    .section ".init"
    .align 3
    .global orin_resolve_core_id
    .type orin_resolve_core_id, %function
orin_resolve_core_id:
    mrs x1, mpidr_el1
    /* keep Aff0/Aff1/Aff2 (bits 23:0).  Aff2 is the cluster on Tegra234 and
     * MUST be kept: 0x10200 (cluster1) and 0x200 (cluster0) share bits 15:0. */
    and x1, x1, #0xffffff
    mov x0, #0
    cbz x1, 9f
    mov x0, #1
    cmp x1, #0x100
    b.eq 9f
    mov x0, #2
    cmp x1, #0x200
    b.eq 9f
    mov x0, #3
    cmp x1, #0x300
    b.eq 9f
    mov x0, #4
    movz x2, #0x0200
    movk x2, #0x1, lsl #16          /* 0x10200 */
    cmp x1, x2
    b.eq 9f
    mov x0, #5
    movz x2, #0x0300
    movk x2, #0x1, lsl #16          /* 0x10300 */
    cmp x1, x2
    b.eq 9f
    mov x0, #6
9:
    ret
    .size orin_resolve_core_id, . - orin_resolve_core_id