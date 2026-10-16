/* SPDX-License-Identifier: MIT */
#include "openbios/base.h"
/* Freestanding compilers may emit these calls for structure copies. */
void *memcpy(void *dst, const void *src, size_t count) {
    return ob_copy(dst, src, count);
}
void *memset(void *dst, int value, size_t count) {
    uint8_t *p = dst;
    while (count--) *p++ = (uint8_t)value;
    return dst;
}
/* 64-bit unsigned division helpers the compiler calls on 32-bit MIPS. */
static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem) {
    uint64_t q = 0, r = 0;
    int i;
    if (!d) { if (rem) *rem = n; return ~(uint64_t)0; }
    for (i = 63; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (uint64_t)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d);
uint64_t __umoddi3(uint64_t n, uint64_t d);
uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod64(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; udivmod64(n, d, &r); return r; }
