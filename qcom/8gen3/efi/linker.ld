/* Q8B UEFI application layout.  Sections are page aligned because
 * mkpefi.py requires a page-aligned RVA for each PE section. */
OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(efi_main)
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
  /DISCARD/ : {
    *(.eh_frame .eh_frame_hdr)
    *(.note* .comment .debug*)
    *(.dynamic .dynsym .dynstr .hash .gnu.hash .gnu.version*)
    *(.interp)
    *(.rela.text* .rela.data* .rela.rodata*)
  }
}
