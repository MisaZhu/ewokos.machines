/*
 * Install the kernel's real exception vector table after the boot MMU is on.
 *
 * This must live in .init and use a literal pool (absolute 64-bit address),
 * NOT a C-level &symbol: the C compiler emits a GOT load for the address and
 * the GOT sits more than 4GB away from .init, outside ADRP's reach.
 */
    .section ".bootvec"
    .align 3
    .global qcom8gen3_install_vectors
    .type qcom8gen3_install_vectors, %function
qcom8gen3_install_vectors:
    ldr x0, =interrupt_table_start
    msr vbar_el1, x0
    isb
    ret
    .size qcom8gen3_install_vectors, . - qcom8gen3_install_vectors
