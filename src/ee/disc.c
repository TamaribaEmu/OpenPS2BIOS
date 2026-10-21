/* SPDX-License-Identifier: MIT */
#include "openbios/disc.h"
#include "openbios/platform.h"
#include "openbios/iso9660.h"
#include "openbios/elf.h"
#include "openbios/rom.h"

/* Load an ELF from the disc ("cdrom0:\\NAME;1" or a bare ISO path). Each
   loadable segment is read straight into RAM, so large non-loaded sections
   such as debug data need not fit anywhere. */
int ob_ee_disc_load(const char *path, uint32_t *entry) {
    static uint8_t head[52 + 64 * 32];
    struct ob_iso iso;
    struct ob_iso_file file;
    uint32_t phoff, count, i, want;
    int result = ob_iso_mount(&iso, ob_ee_disc_read, 0, UINT32_MAX), executable = 0;
    if (!result) result = ob_iso_find(&iso, path, &file);
    if (result) return result;
    ob_puts("[openbios:disc] loading ELF bytes="); ob_puthex(file.size); ob_puts("\n");
    want = file.size < sizeof(head) ? file.size : sizeof(head);
    if (want < 52) return OB_EFORMAT;
    result = ob_iso_read(&iso, &file, 0, head, want);
    if (result) return result;
    if (!ob_equal(head, "\177ELF\1\1\1", 7) || ob_le16(head + 16) != 2 || ob_le16(head + 18) != 8 ||
        ob_le16(head + 42) != 32) return OB_EFORMAT;
    phoff = ob_le32(head + 28);
    count = ob_le16(head + 44);
    *entry = ob_le32(head + 24);
    if (!count || count > 64 || phoff > want || count * 32 > want - phoff || (*entry & 3)) return OB_EFORMAT;
    /* Validate every segment before writing any. */
    for (i = 0; i < count; ++i) {
        const uint8_t *ph = head + phoff + i * 32;
        uint32_t off = ob_le32(ph + 4), addr = ob_le32(ph + 8), filesz = ob_le32(ph + 16),
                 memsz = ob_le32(ph + 20);
        if (ob_le32(ph) != 1 || (!memsz && !filesz)) continue;
        /* Scratchpad segment (MK Deadly Alliance). */
        if (addr >= 0x70000000u && ob_range(addr - 0x70000000u, memsz, 0x4000u) &&
            ob_range(off, filesz, file.size) && filesz <= memsz) continue;
        /* No RAM past 32 MB on retail. Soulcalibur II has a .text2 there. */
        if (addr >= 0x02000000u) {
            ob_puts("[openbios:disc] skipping segment above RAM at "); ob_puthex(addr); ob_puts("\n");
            continue;
        }
        if (filesz > memsz || !ob_range(off, filesz, file.size) || addr < 0x00100000u ||
            !ob_range(addr, memsz, 0x02000000u)) return OB_EBOUNDS;
        if (*entry >= addr && *entry - addr < filesz) executable = 1;
    }
    if (!executable) return OB_EFORMAT;
    for (i = 0; i < count; ++i) {
        const uint8_t *ph = head + phoff + i * 32;
        uint32_t off = ob_le32(ph + 4), addr = ob_le32(ph + 8), filesz = ob_le32(ph + 16),
                 memsz = ob_le32(ph + 20);
        uint8_t *dest = (uint8_t *)(uintptr_t)(0xa0000000u | addr);
        int spr = addr >= 0x70000000u && ob_range(addr - 0x70000000u, memsz, 0x4000u);
        if (ob_le32(ph) != 1 || (!memsz && !filesz) || (addr >= 0x02000000u && !spr)) continue;
        if (spr) dest = (uint8_t *)(uintptr_t)addr; /* mapped by the kernel TLB */
        if (filesz) {
            result = ob_iso_read(&iso, &file, off, dest, filesz);
            if (result) return result;
        }
        ob_zero(dest + filesz, memsz - filesz);
    }
    return OB_OK;
}

/* Load an ELF stored in the ROM directory ("OSDSYS" or "OSDSYS;1"). */
int ob_ee_rom_load(const char *name, uint32_t *entry) {
    struct ob_rom_file file;
    struct ob_elf_info info;
    char clean[16];
    unsigned n = 0;
    int result;
    while (name[n] && name[n] != ';' && n < sizeof(clean) - 1) { clean[n] = name[n]; ++n; }
    clean[n] = 0;
    result = ob_rom_find((const void *)0xbfc00000u, OB_ROM_SIZE, clean, &file);
    if (!result) result = ob_elf_load(file.data, file.size, (void *)0xa0100000u, 0x00100000u,
                                      0x01f00000u, &info);
    if (!result) *entry = info.entry;
    return result;
}

int ob_ee_disc_boot(uint32_t *entry) {
    struct ob_iso iso;
    char path[256], argv0[264];
    const char *args[1];
    unsigned i = 0, j = 0;
    int result;
    ob_puts("[openbios:disc] mounting ISO9660\n");
    result = ob_iso_mount(&iso, ob_ee_disc_read, 0, UINT32_MAX);
    if (!result) ob_puts("[openbios:disc] mounted\n");
    if (!result) result = ob_iso_boot_path(&iso, path, sizeof(path));
    if (result) return result;
    ob_puts("[openbios:disc] BOOT2="); ob_puts(path); ob_puts("\n");
    result = ob_ee_disc_load(path, entry);
    if (result) return result;
    /* argv[0] is the boot path in its "cdrom0:\\NAME;1" form. */
    while (path[i] && path[i] != ':' && j < sizeof(argv0) - 2) argv0[j++] = path[i++];
    if (path[i] == ':') {
        argv0[j++] = path[i++];
        if (path[i] != '\\' && path[i] != '/') argv0[j++] = '\\';
    }
    while (path[i] && j < sizeof(argv0) - 1) argv0[j++] = path[i++];
    argv0[j] = 0;
    args[0] = argv0;
    ob_ee_set_exec_args(1, args);
    return OB_OK;
}
