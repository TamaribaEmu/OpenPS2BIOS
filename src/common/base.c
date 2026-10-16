/* SPDX-License-Identifier: MIT */
#include "openbios/base.h"

uint16_t ob_le16(const void *p) {
    const uint8_t *s = p;
    return (uint16_t)(s[0] | ((uint16_t)s[1] << 8));
}
uint32_t ob_le32(const void *p) {
    const uint8_t *s = p;
    return ob_word32((uint32_t)s[0] | ((uint32_t)s[1] << 8) |
        ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24));
}
typedef uint32_t ob_word __attribute__((may_alias));
/* Forward copy (overlap allowed when dst < src), by words when both are
   word aligned: kernel paths copy whole register frames. */
void *ob_copy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (!(((uintptr_t)d | (uintptr_t)s) & 3)) {
        ob_word *dw = (ob_word *)(void *)d;
        const ob_word *sw = (const ob_word *)(const void *)s;
        for (; n >= 16; n -= 16, dw += 4, sw += 4) {
            ob_word a = sw[0], b = sw[1], c = sw[2], e = sw[3];
            dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
        }
        for (; n >= 4; n -= 4) *dw++ = *sw++;
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
    }
    while (n--) *d++ = *s++;
    return dst;
}
void *ob_zero(void *dst, size_t n) {
    uint8_t *d = dst;
    if (!((uintptr_t)d & 3)) {
        ob_word *dw = (ob_word *)(void *)d;
        for (; n >= 16; n -= 16, dw += 4) dw[0] = dw[1] = dw[2] = dw[3] = 0;
        for (; n >= 4; n -= 4) *dw++ = 0;
        d = (uint8_t *)dw;
    }
    while (n--) *d++ = 0;
    return dst;
}
int ob_equal(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) if (*x++ != *y++) return 0;
    return 1;
}
int ob_range(size_t offset, size_t count, size_t size) {
    return offset <= size && count <= size - offset;
}
