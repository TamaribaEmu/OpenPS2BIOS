/* SPDX-License-Identifier: MIT */
#include "openbios/irx.h"

#define R_MIPS_NONE 0
#define R_MIPS_32 2
#define R_MIPS_26 4
#define R_MIPS_HI16 5
#define R_MIPS_LO16 6

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int ob_irx_parse(const void *file, uint32_t size, struct ob_irx_info *info) {
    const uint8_t *f = file;
    uint32_t phoff, shoff, i, have_mod = 0, have_load = 0;
    uint16_t phentsize, phnum, shentsize, shnum;
    if (!f || !info || size < 52) return OB_EINVAL;
    ob_zero(info, sizeof(*info));
    if (ob_le32(f) != 0x464c457fu || f[4] != 1 || f[5] != 1) return OB_EFORMAT;
    if (le16(f + 16) != OB_IRX_ET || le16(f + 18) != 8) return OB_EFORMAT;
    info->entry = ob_le32(f + 24);
    phoff = ob_le32(f + 28);
    shoff = ob_le32(f + 32);
    phentsize = le16(f + 42); phnum = le16(f + 44);
    shentsize = le16(f + 46); shnum = le16(f + 48);
    if (phentsize < 32 || !ob_range(phoff, (uint32_t)phentsize * phnum, size)) return OB_EBOUNDS;
    for (i = 0; i < phnum; ++i) {
        const uint8_t *p = f + phoff + i * phentsize;
        uint32_t type = ob_le32(p), off = ob_le32(p + 4), vaddr = ob_le32(p + 8);
        uint32_t filesz = ob_le32(p + 16), memsz = ob_le32(p + 20);
        if (!ob_range(off, filesz, size)) return OB_EBOUNDS;
        if (type == OB_IRX_PT_IOPMOD) {
            const uint8_t *m = f + off;
            uint32_t n;
            if (filesz < 26 || have_mod++) return OB_EFORMAT;
            info->moduleinfo = ob_le32(m);
            info->entry = ob_le32(m + 4);
            info->gp = ob_le32(m + 8);
            info->text_size = ob_le32(m + 12);
            info->data_size = ob_le32(m + 16);
            info->bss_size = ob_le32(m + 20);
            info->version = le16(m + 24);
            for (n = 0; n + 26 < filesz && n + 1 < sizeof(info->name) && m[26 + n]; ++n)
                info->name[n] = (char)m[26 + n];
        } else if (type == 1) {
            if (have_load++ || vaddr != 0 || filesz > memsz || memsz > 0x00200000u)
                return OB_EFORMAT;
            info->file_offset = off;
            info->file_size = filesz;
            info->mem_size = memsz;
        }
    }
    if (!have_mod || !have_load || info->entry >= info->mem_size) return OB_EFORMAT;
    if (shnum && (shentsize < 40 || !ob_range(shoff, (uint32_t)shentsize * shnum, size)))
        return OB_EBOUNDS;
    for (i = 0; i < shnum; ++i) {
        const uint8_t *s = f + shoff + i * shentsize;
        uint32_t type = ob_le32(s + 4), off = ob_le32(s + 16), bytes = ob_le32(s + 20);
        if (type == 4) return OB_ENOSYS; /* RELA is not used by IRX modules. */
        if (type != 9 || !bytes) continue;
        if (bytes % 8 || !ob_range(off, bytes, size)) return OB_EBOUNDS;
        if (info->rel_count == OB_IRX_MAX_RELS) return OB_ENOSPC;
        info->rels[info->rel_count].offset = off;
        info->rels[info->rel_count++].count = bytes / 8;
    }
    return OB_OK;
}

static int fits(const struct ob_irx_info *info, uint32_t where) {
    return !(where & 3) && ob_range(where, 4, info->mem_size);
}
int ob_irx_load(const void *file, uint32_t size, const struct ob_irx_info *info,
                void *dest, uint32_t base) {
    const uint8_t *f = file;
    uint8_t *image = dest;
    unsigned r;
    if (!f || !info || !image || (base & 3)) return OB_EINVAL;
    if (!ob_range(info->file_offset, info->file_size, size)) return OB_EBOUNDS;
    ob_copy(image, f + info->file_offset, info->file_size);
    ob_zero(image + info->file_size, info->mem_size - info->file_size);
    for (r = 0; r < info->rel_count; ++r) {
        const uint8_t *rel = f + info->rels[r].offset;
        uint32_t i, hi[16], nhi = 0;
        for (i = 0; i < info->rels[r].count; ++i, rel += 8) {
            uint32_t where = ob_le32(rel), type = ob_le32(rel + 4) & 0xff, word;
            if (type == R_MIPS_NONE) continue;
            /* Some old linkers pad HI16/LO16 pairs with an entry at offset -4
               (LOTR Fellowship's RSRCIOP.IRX has 19 of them): a HI16 shared by
               several LO16s, or a LO16 shared by several HI16s. Nothing to
               patch there. The loader in retail just pokes the word before the
               module. A padded LO16 still ends the pending HI16s, with low 0. */
            if (where == 0xfffffffcu && type == R_MIPS_HI16) continue;
            if (where == 0xfffffffcu && type == R_MIPS_LO16) {
                uint32_t k;
                for (k = 0; k < nhi; ++k) {
                    uint32_t h = ob_le32(image + hi[k]);
                    uint32_t value = (h << 16) + base;
                    put32(image + hi[k], (h & 0xffff0000u) | (((value + 0x8000u) >> 16) & 0xffff));
                }
                nhi = 0;
                continue;
            }
            if (!fits(info, where)) return OB_EBOUNDS;
            word = ob_le32(image + where);
            switch (type) {
            case R_MIPS_32:
                put32(image + where, word + base);
                break;
            case R_MIPS_26:
                put32(image + where, (word & 0xfc000000u) |
                    (((word & 0x03ffffffu) + (base >> 2)) & 0x03ffffffu));
                break;
            case R_MIPS_HI16:
                /* Applied when the following LO16 supplies the low half. */
                if (nhi == sizeof(hi) / sizeof(hi[0])) return OB_ENOSPC;
                hi[nhi++] = where;
                break;
            case R_MIPS_LO16: {
                int32_t low = (int16_t)(word & 0xffff);
                uint32_t k;
                for (k = 0; k < nhi; ++k) {
                    uint32_t h = ob_le32(image + hi[k]);
                    uint32_t value = (h << 16) + (uint32_t)low + base;
                    put32(image + hi[k], (h & 0xffff0000u) | (((value + 0x8000u) >> 16) & 0xffff));
                }
                nhi = 0;
                put32(image + where, (word & 0xffff0000u) | (((uint32_t)low + base) & 0xffff));
                break;
            }
            default:
                return OB_ENOSYS;
            }
        }
        if (nhi) return OB_EFORMAT; /* HI16 without a matching LO16. */
    }
    return OB_OK;
}

int ob_irx_version_ok(uint16_t import_version, uint16_t export_version) {
    /* Only the major version must match; modules routinely import a newer
       minor version than the exporting module they ship with provides. */
    return (import_version >> 8) == (export_version >> 8);
}

int ob_irx_link(void *image_ptr, uint32_t size, ob_irx_resolver resolve, void *ctx,
                uint32_t *failed_table, uint32_t *failed_index) {
    uint8_t *image = image_ptr;
    uint32_t w;
    int tables = 0;
    if (!image || !resolve || (size & 3)) return OB_EINVAL;
    for (w = 0; w + 20 <= size; w += 4) {
        uint32_t p;
        char name[8];
        uint16_t version;
        unsigned k;
        if (ob_le32(image + w) != OB_IRX_IMPORT_MAGIC || ob_le32(image + w + 4)) continue;
        version = le16(image + w + 8);
        for (k = 0; k < 8; ++k) name[k] = (char)image[w + 12 + k];
        for (p = w + 20; ; p += 8) {
            uint32_t jump, ins, target;
            if (p + 8 > size) return OB_EFORMAT;
            jump = ob_le32(image + p);
            ins = ob_le32(image + p + 4);
            if (!jump && !ins) break;
            if ((jump != OB_IRX_STUB_RETURN && (jump >> 26) != 2) || (ins >> 16) != 0x2400)
                return OB_EFORMAT;
            target = resolve(ctx, name, version, ins & 0xffff);
            if (!target || (target & 3)) {
                if (failed_table) *failed_table = w;
                if (failed_index) *failed_index = ins & 0xffff;
                return OB_ENOTFOUND;
            }
            put32(image + p, 0x08000000u | ((target >> 2) & 0x03ffffffu)); /* j target */
        }
        ++tables;
        w = p; /* Continue after the terminating pair. */
    }
    return tables;
}
