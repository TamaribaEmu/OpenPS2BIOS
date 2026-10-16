/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_ROM_H
#define OPENBIOS_ROM_H
#include "base.h"
#define OB_ROM_SIZE (4u * 1024u * 1024u)
#define OB_ROMDIR_OFFSET 0x1000u
struct ob_rom_file { const uint8_t *data; uint32_t size; };
int ob_rom_find(const void *rom, size_t size, const char *name,
                struct ob_rom_file *file);
#endif
