/*
 * EwokOS AArch64 linker script for machines with DRAM not based at 0.
 *
 * Q8B DRAM starts at 0x80000000, therefore:
 *   - .init is linked at its physical load address (0x80080000) so the
 *     pre-MMU code (adr/literal stack references) works unchanged;
 *   - the rest of the kernel is linked in the high half at
 *     KERNEL_BASE + (physical - RAM_BASE), which is what V2P/P2V expect.
 *
 * The UEFI stub copies the raw binary to LOAD_ADDRESS, so the value below
 * must match config.mk and boot/uefi/main.c.
 */
ENTRY(__entry)

base_address = 0x4000000000;
/* Physical DRAM base; overridden by --defsym=phys_base=$(RAM_BASE) for the
 * QEMU test variant (RAM_BASE=0x40000000). */
PROVIDE(phys_base = 0x80000000);

PHDRS
{
  init   PT_LOAD FLAGS(5); /* read-only, executable*/
  text   PT_LOAD FLAGS(5); /* read-only, executable*/
}

SECTIONS
{
	/* .init keeps the physical load address as its VMA: the pre-MMU entry
	 * code uses PC-relative addressing and must not see a high-half VMA. */
	.init start_address : AT(start_address) ALIGN(page_align_size)
	{
    KEEP(*(.init .init.*))
		*/start*.o(.text .text.*)
		*/start*.o(.rodata .rodata.*)
		*/start*.o(.data .data.*)
		*/start*.o(.bss .bss.*)
		/* Q8B UART bring-up runs before the MMU and must stay physical. */
		*(.text.q8b_early .text.q8b_early.*)
		/* late .init helpers that must not precede __entry */
		*(.bootvec)
	} : init
	. = . + 0x4000;
	_svc_start_stack = .;
	_init_end = ALIGN(page_align_size);

	/* Kernel proper: high-half VMA, physical LMA right after .init. */
	PROVIDE(_kernel_start = base_address + (_init_end - phys_base));

	.text base_address + (_init_end - phys_base) : AT(_init_end) ALIGN(page_align_size)
	{
		KEEP(*(.text))
		*(.text)
	} : text
	. = ALIGN(page_align_size);

	.data : { /** data segment */
		*(.data*)
	}
	. = ALIGN(page_align_size);

	_bss_start = .;
	.bss :
	{
		*(.bss*)
	}
	. = ALIGN(page_align_size);
	_bss_end = .;

	. = . + 0x4000;
	_irq_stack = .;

	. = . + 0x4000;
	_abt_stack = .;

	. = . + 0x4000;
	_undef_stack = .;

	. = . + 0x4000;
	_svc_stack = .;
	/* One private SVC stack per additional core (see boot.S slave_core). */
	. = . + (0x4000 * 7);
	PROVIDE(_kernel_sp = .);

	. = ALIGN(page_align_size);
	PROVIDE(_kernel_end = .);
}
