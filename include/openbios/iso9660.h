/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_ISO9660_H
#define OPENBIOS_ISO9660_H

#include "base.h"

#define OB_ISO_SECTOR_SIZE 2048u
#define OB_ISO_SYSTEM_CNF_MAX 8192u
#define OB_ISO_PATH_MAX 256u

typedef int (*ob_iso_sector_reader)(void *ctx, uint32_t lba,
                                    uint8_t out[OB_ISO_SECTOR_SIZE]);

struct ob_iso {
    ob_iso_sector_reader read_sector;
    void *ctx;
    uint32_t sector_count;
    uint32_t root_lba;
    uint32_t root_size;
};

struct ob_iso_file {
    uint32_t lba;
    uint32_t size;
};

#define OB_ISO_NAME_MAX 64u
/* A directory record: flags bit 1 marks a directory; date is the recording
   time (years since 1900, month, day, hour, minute, second, GMT offset). */
struct ob_iso_entry {
    uint32_t lba;
    uint32_t size;
    uint8_t flags;
    uint8_t date[7];
    char name[OB_ISO_NAME_MAX];
};

int ob_iso_mount(struct ob_iso *iso, ob_iso_sector_reader read_sector,
                 void *ctx, uint32_t sector_count);
int ob_iso_find(const struct ob_iso *iso, const char *path,
                struct ob_iso_file *file);
/* Find a file or directory ("" or "/" is the root). */
int ob_iso_lookup(const struct ob_iso *iso, const char *path, struct ob_iso_entry *entry);
/* Iterate a directory's records (including "." and ".."); *position starts at 0. */
int ob_iso_dir_next(const struct ob_iso *iso, uint32_t dir_lba, uint32_t dir_size,
                    uint32_t *position, struct ob_iso_entry *entry);
int ob_iso_read(const struct ob_iso *iso, const struct ob_iso_file *file,
                uint32_t offset, void *dst, uint32_t size);
int ob_iso_boot_path(const struct ob_iso *iso, char *out, size_t capacity);

#endif
