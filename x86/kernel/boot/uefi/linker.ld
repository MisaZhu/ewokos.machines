/* bootloader ELF 布局 — 之后由 objcopy -O pe-x86-64 转成 UEFI 可执行
 * 段间 0x1000 对齐, 入口 RVA 非零 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
SECTIONS
{
  . = 0x1000;
  .text : { *(.text .text.*) }
  . = ALIGN(0x1000);
  .rodata : { *(.rodata .rodata.*) }
  . = ALIGN(0x1000);
  .data : { *(.data .data.* .got .got.* .got.plt) }
  . = ALIGN(0x1000);
  .bss : { *(.bss .bss.* COMMON) }
  .rela.dyn : { *(.rela.dyn) }
  /DISCARD/ : { *(.eh_frame .eh_frame_hdr .note* .comment .debug* .rela.text* .dynamic .dynsym .dynstr .got .interp .hash .gnu.hash .gnu.version*) }
}
