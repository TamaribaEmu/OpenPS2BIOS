/* SPDX-License-Identifier: MIT */
#include "openbios/ee.h"
#include "openbios/rom.h"
#include "openbios/elf.h"
#include "openbios/platform.h"
#include "openbios/disc.h"

void ob_ee_main(void) {
    struct ob_rom_file boot;
    struct ob_elf_info executable;
    int result;
    ob_puts("[openbios:ee] OpenPS2BIOS " OB_VERSION " ready\n");
    {   /* COP0 Config: kseg0 cacheable (K0 = 3), branch prediction,
           non-blocking loads, both caches and dual issue enabled. */
        uint32_t config;
        __asm__ volatile ("mfc0 %0, $16" : "=r"(config));
        config = (config & ~7u) | 3u | (1u << 12) | (1u << 13) | (1u << 16) | (1u << 17) | (1u << 18);
        __asm__ volatile ("mtc0 %0, $16\n\t.word 0x0000040f" :: "r"(config)); /* sync.p */
    }
    ob_ee_vectors_init();
    ob_ee_tlb_init();
    ob_ee_kernel_init();
    /* Reset the GS (CSR.RESET) before any program runs. */
    *(volatile uint64_t *)0xb2001000u = 0x200;
    result = ob_rom_find((const void *)0xbfc00000u, OB_ROM_SIZE, "BOOTELF", &boot);
    if (result == OB_ENOTFOUND) {
        /* No embedded program: EELOAD runs the boot screen (rom0:OSDSYS),
           which waits for a disc. */
        ob_ee_kernel_strict(1);
        ob_ee_install_eeload();
        ob_puts("[openbios:ee] starting EELOAD\n");
        ob_ee_interrupts_enable();
        ob_ee_run_eeload();
    } else if (!result) {
        result = ob_elf_load(boot.data, boot.size,
            (void *)0xa0100000u, 0x00100000u, 0x01f00000u, &executable);
    }
    if (result) {
        ob_puts("[openbios:ee] boot rejected error=");
        ob_puthex((uint32_t)result); ob_puts("\n"); ob_halt();
    }
    __asm__ volatile ("sync" ::: "memory");
    ob_puts("[openbios:ee] executing ELF entry="); ob_puthex(executable.entry);
    ob_puts("\n");
    ob_ee_interrupts_enable();
    ((void (*)(void))(uintptr_t)executable.entry)();
    ob_puts("[openbios:ee] ELF returned\n");
    ob_halt();
}
