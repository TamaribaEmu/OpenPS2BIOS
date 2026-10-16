/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_IRX_H
#define OPENBIOS_IRX_H
#include "base.h"

/* IOP relocatable module (IRX) format: ELF32 type 0xff80 with a .iopmod program
   header (0x70000080), one load segment linked at 0 and REL relocations. */
#define OB_IRX_ET 0xff80u
#define OB_IRX_PT_IOPMOD 0x70000080u
#define OB_IRX_IMPORT_MAGIC 0x41e00000u
#define OB_IRX_EXPORT_MAGIC 0x41c00000u
#define OB_IRX_STUB_RETURN 0x03e00008u /* jr $ra, patched to j <function> */
#define OB_IRX_MAX_RELS 8

struct ob_irx_info {
    uint32_t file_offset, file_size, mem_size; /* load segment */
    uint32_t entry, gp, moduleinfo;
    uint32_t text_size, data_size, bss_size;
    uint16_t version;
    char name[32];
    unsigned rel_count;
    struct { uint32_t offset, count; } rels[OB_IRX_MAX_RELS];
};
/* Library export table as it appears in memory. */
struct ob_irx_exports {
    uint32_t magic;
    struct ob_irx_exports *next;
    uint16_t version, mode;
    char name[8];
    uint32_t functions[]; /* zero-terminated */
};
/* Resolve library/version/function index to an absolute address, or 0. */
typedef uint32_t (*ob_irx_resolver)(void *ctx, const char name[8], uint16_t version,
                                   uint32_t index);

int ob_irx_parse(const void *file, uint32_t size, struct ob_irx_info *info);
/* Copy the load segment to dest (mem_size bytes), clear bss and relocate for
   execution at base. dest and the file must not overlap. */
int ob_irx_load(const void *file, uint32_t size, const struct ob_irx_info *info,
                void *dest, uint32_t base);
/* Patch every import stub in a loaded image. Returns the number of tables linked,
   or an error. On error *failed_index receives the unresolved stub's position. */
int ob_irx_link(void *image, uint32_t size, ob_irx_resolver resolve, void *ctx,
                uint32_t *failed_table, uint32_t *failed_index);
/* True when an import with this version may use an export with that version
   (same major version). */
int ob_irx_version_ok(uint16_t import_version, uint16_t export_version);
#endif
