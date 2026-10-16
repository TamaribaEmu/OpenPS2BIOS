/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_BASE_H
#define OPENBIOS_BASE_H
#include <stdint.h>
#include <stddef.h>

enum ob_error {
    OB_OK = 0, OB_EINVAL = -1, OB_EBOUNDS = -2, OB_EFORMAT = -3,
    OB_ENOTFOUND = -4, OB_ENOSYS = -5, OB_ENOSPC = -6,
    OB_EBUSY = -7, OB_EEMPTY = -8
};

static inline uint32_t ob_word32(uint32_t value) {
#ifdef __mips__
    /* R5900 branches compare 64 bits. Normalize after generic MIPS32 sequences. */
    __asm__ volatile ("sll %0, %0, 0" : "+r"(value));
#endif
    return value;
}

uint16_t ob_le16(const void *p);
uint32_t ob_le32(const void *p);
void *ob_copy(void *dst, const void *src, size_t n);
void *ob_zero(void *dst, size_t n);
int ob_equal(const void *a, const void *b, size_t n);
int ob_range(size_t offset, size_t count, size_t size);
#endif
