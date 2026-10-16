/* SPDX-License-Identifier: MIT */
#include "openbios/elf.h"

int ob_elf_load(const void *image, size_t size, void *memory,
                uint32_t base, uint32_t capacity, struct ob_elf_info *info) {
    const uint8_t *elf = image;
    uint8_t *ram = memory;
    uint32_t phoff, entry, low = UINT32_MAX, high = 0;
    uint16_t count, stride;
    unsigned i, j;
    int executable_entry = 0;
    if (!elf || !ram || !info) return OB_EINVAL;
    if (size < 52 || !ob_equal(elf, "\177ELF\1\1\1", 7) ||
        ob_le16(elf + 16) != 2 || ob_le16(elf + 18) != 8 ||
        ob_le32(elf + 20) != 1 || ob_le16(elf + 40) != 52)
        return OB_EFORMAT;
    phoff = ob_le32(elf + 28);
    stride = ob_le16(elf + 42);
    count = ob_le16(elf + 44);
    entry = ob_le32(elf + 24);
    if (stride != 32 || count == 0 || count > 128 || (entry & 3) ||
        !ob_range(phoff, (size_t)count * stride, size)) return OB_EFORMAT;
    for (i = 0; i < count; ++i) {
        const uint8_t *p = elf + phoff + i * stride;
        uint32_t off = ob_le32(p + 4), addr = ob_le32(p + 8);
        uint32_t filesz = ob_le32(p + 16), memsz = ob_le32(p + 20);
        uint32_t flags = ob_le32(p + 24), align = ob_le32(p + 28);
        if (ob_le32(p) != 1) continue;
        if (!memsz && !filesz) continue; /* empty segments place nothing */
        if (filesz > memsz || !ob_range(off, filesz, size) || addr < base ||
            !ob_range(addr - base, memsz, capacity) || memsz > UINT32_MAX - addr)
            return OB_EBOUNDS;
        if (align > 1 && ((align & (align - 1)) ||
            (addr & (align - 1)) != (off & (align - 1)))) return OB_EFORMAT;
        /* Reject overlapping destinations; load order must not change the result. */
        for (j = 0; j < i; ++j) {
            const uint8_t *q = elf + phoff + j * stride;
            uint32_t other = ob_le32(q + 8), length = ob_le32(q + 20);
            if (ob_le32(q) == 1 && memsz && length &&
                addr < other + length && other < addr + memsz) return OB_EFORMAT;
        }
        if (memsz) {
            if (addr < low) low = addr;
            if (addr + memsz > high) high = addr + memsz;
            if ((flags & 1) && entry >= addr && entry - addr < filesz)
                executable_entry = 1;
        }
    }
    if (!executable_entry) return OB_EFORMAT;
    /* Source is required to be disjoint from destination RAM. */
    if ((uintptr_t)elf <= UINTPTR_MAX - size &&
        (uintptr_t)ram <= UINTPTR_MAX - capacity &&
        (uintptr_t)elf < (uintptr_t)ram + capacity &&
        (uintptr_t)ram < (uintptr_t)elf + size) return OB_EINVAL;
    for (i = 0; i < count; ++i) {
        const uint8_t *p = elf + phoff + i * stride;
        uint32_t addr = ob_le32(p + 8), filesz = ob_le32(p + 16);
        if (ob_le32(p) != 1 || (!filesz && !ob_le32(p + 20))) continue;
        ob_copy(ram + (addr - base), elf + ob_le32(p + 4), filesz);
        ob_zero(ram + (addr - base) + filesz, ob_le32(p + 20) - filesz);
    }
    info->entry = entry;
    info->low = low;
    info->high = high;
    return OB_OK;
}
