/* SPDX-License-Identifier: MIT */
OUTPUT_ARCH(mips)
ENTRY(_start)
SECTIONS {
    . = 0xbfc00000;
    .text : { *(.text.start) *(.text .text.*) }
    .got : { *(.got) }
    . = 0xbfc01000 - 1;
    .padding : { BYTE(0); }
    ASSERT(SIZEOF(.text) + SIZEOF(.got) <= 0x1000, "RESET exceeds ROMDIR offset")
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr) *(.comment) *(.note*) }
}
