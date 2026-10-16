/* SPDX-License-Identifier: MIT */
#include "openbios/rom.h"

int ob_rom_find(const void *image, size_t size, const char *name,
                struct ob_rom_file *file) {
    const uint8_t *rom = image;
    size_t offset = 0, dir = OB_ROMDIR_OFFSET, limit;
    size_t name_len = 0;
    struct ob_rom_file found = {0, 0};
    if (!rom || !name || !file) return OB_EINVAL;
    while (name_len < 10 && name[name_len]) ++name_len;
    if (!name_len || name_len > 9) return OB_EINVAL;
    if (!ob_range(dir, 32, size) || !ob_equal(rom + dir, "RESET\0", 6) ||
        ob_le32(rom + dir + 12) != OB_ROMDIR_OFFSET ||
        !ob_equal(rom + dir + 16, "ROMDIR\0", 7)) return OB_EFORMAT;
    limit = ob_le32(rom + dir + 28);
    if (limit < 48 || (limit & 15) || !ob_range(dir, limit, size))
        return OB_EFORMAT;
    limit += dir;
    for (; dir < limit; dir += 16) {
        const uint8_t *entry = rom + dir;
        uint32_t len = ob_le32(entry + 12);
        size_t padded = ((size_t)len + 15) & ~(size_t)15;
        if (!entry[0]) {
            if (dir != limit - 16 || !ob_equal(entry,
                "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16)) return OB_EFORMAT;
            if (!found.data) return OB_ENOTFOUND;
            *file = found;
            return OB_OK;
        }
        if (padded < len || !ob_range(offset, padded, size)) return OB_EBOUNDS;
        if (ob_equal(entry, name, name_len) && entry[name_len] == 0) {
            if (found.data) return OB_EFORMAT;
            found.data = rom + offset;
            found.size = len;
        }
        offset += padded;
    }
    return OB_EFORMAT;
}
