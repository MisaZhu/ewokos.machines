ENTRY(__entry)

base_address = 0x80000000;

PHDRS
{
  init   PT_LOAD FLAGS(5);
  text   PT_LOAD FLAGS(5);
}

SECTIONS
{
	.init start_address : AT(start_address) ALIGN(4096)
	{
		KEEP(*(.multiboot))
		KEEP(*(.init .init.*))
		KEEP(*(.uefi .uefi.*))
		*/start*.o(.text .text.*)
		*/start*.o(.rodata .rodata.*)
		*/start*.o(.data .data.*)
		*/start*.o(.bss .bss.*)
	} : init
	. = . + 0x4000;
	_svc_start_stack = .;
	_init_end = ALIGN(4096);

	PROVIDE(_kernel_start = base_address + _init_end - start_address);

	.text base_address + _init_end - start_address : AT(_init_end) ALIGN(4096)
	{
		KEEP(*(.text .text.*))
	} : text
	. = ALIGN(4096);

	.data : {
		*(.data*)
	}
	. = ALIGN(4096);

	_bss_start = .;
	.bss : {
		*(.bss*)
		*(COMMON)
	}
	. = ALIGN(4096);
	_bss_end = .;

	/* AHCI DMA 结构区(命令列表/FIS/命令表/数据缓冲): 必须位于内核镜像
	 * 内, 重映射后的内核页表才可直访; 物理地址 = V2P(此处 VA) */
	. = ALIGN(4096);
	_ahci_dma_base = .;
	. = . + 0x1000;

	/* NVMe DMA 区: admin/IO 队列 + IDENTIFY 缓冲 + 数据缓冲, 页对齐 */
	. = ALIGN(4096);
	_nvme_dma_base = .;
	. = . + 0x6000;

	. = . + 0x4000;
	_irq_stack = .;

	. = . + 0x4000;
	_abt_stack = .;

	. = . + 0x4000;
	_undef_stack = .;

	. = . + 0x4000;
	_svc_stack = .;
	PROVIDE(_kernel_sp = .);

	. = ALIGN(4096);
	PROVIDE(_kernel_end = .);
}
