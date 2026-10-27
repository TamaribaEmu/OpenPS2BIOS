/* SPDX-License-Identifier: MIT */
OUTPUT_ARCH(mips)
ENTRY(_start)
SECTIONS {
    . = 0x00001000; /* kuseg, so IRX import stubs (j) can reach exports; just
                       above the exception vectors to leave RAM for modules. */
    .text : { KEEP(*(.text.start)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) *(.sdata .sdata.*) }
    .got : { *(.got) }
    . = ALIGN(16);
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;
    ASSERT(. < 0x0003af00, "runtime exceeds reserved low RAM")
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr) *(.comment) *(.note*) }
}
