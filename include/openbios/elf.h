/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_ELF_H
#define OPENBIOS_ELF_H
#include "base.h"
/* Validate every segment before writing anything. Addresses are physical EE RAM. */
struct ob_elf_info { uint32_t entry; uint32_t low; uint32_t high; };
int ob_elf_load(const void *image, size_t image_size, void *ram,
                uint32_t ram_base, uint32_t ram_size, struct ob_elf_info *info);
#endif
