/* SPDX-License-Identifier: MIT */
OUTPUT_ARCH(mips)
ENTRY(_start)
SECTIONS {
    . = 0xa0010000;
    .text : { KEEP(*(.text.start)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) *(.sdata .sdata.*) }
    .got : { *(.got) }
    . = ALIGN(16);
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;
    ASSERT(. < 0xa0070000, "runtime exceeds reserved low RAM")
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr) *(.comment) *(.note*) }
}
