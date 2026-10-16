/* SPDX-License-Identifier: MIT */
OUTPUT_ARCH(mips)
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(7); }
SECTIONS {
    . = 0x00100000;
    .text : { *(.text.start) *(.text*) *(.rodata*) } :text
    .data : { *(.data*) *(.sdata*) *(.got) } :text
    .bss : { *(.bss*) *(.sbss*) *(COMMON) } :text
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr) *(.comment) *(.note*) }
}
