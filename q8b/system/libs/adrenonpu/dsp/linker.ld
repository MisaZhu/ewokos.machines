/* Q8B QuRT loader: two page-aligned LOADs, headers in the executable LOAD.
 * The generic LLD layout's non-page-aligned data file offset is not accepted
 * by the Q8B firmware loader. No vendor linker script is included. */
PHDRS {
  text PT_LOAD FILEHDR PHDRS FLAGS(5);
  data PT_LOAD FLAGS(6);
  dynamic PT_DYNAMIC FLAGS(6);
}
SECTIONS {
  . = SIZEOF_HEADERS;
  .dynsym : { *(.dynsym) } :text
  .dynstr : { *(.dynstr) } :text
  .hash : { *(.hash) } :text
  .rela.dyn : { *(.rela.dyn) } :text
  .rela.plt : { *(.rela.plt) } :text
  .init : { *(.init) } :text
  .plt : { *(.plt) } :text
  .text : { *(.text*) } :text
  .fini : { *(.fini) } :text
  .rodata : { *(.rodata*) } :text
  .eh_frame : { *(.eh_frame*) } :text
  . = ALIGN(0x1000);
  .dynamic : { *(.dynamic) } :data :dynamic
  .data.rel.ro : { *(.data.rel.ro*) } :data
  .data : { *(.data*) } :data
  .got : { *(.got) } :data
  .got.plt : { *(.got.plt) } :data
  .bss : { *(.bss*) *(COMMON) } :data
  /DISCARD/ : { *(.comment) *(.hexagon.attributes) *(.note.GNU-stack) }
}
