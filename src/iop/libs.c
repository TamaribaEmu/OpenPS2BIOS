/* SPDX-License-Identifier: MIT */
/* Built-in IOP libraries exported to IRX modules. Function indices follow the
   standard export tables; unimplemented indices stay null so linking fails with a
   clear "unresolved import" instead of calling a stub. */
#include "openbios/iop.h"
#include "openbios/format.h"
#include "openbios/platform.h"

int ob_iop_register_builtin(struct ob_irx_exports *exports, uint32_t count);

#define LIBRARY(var, text, major, minor, size) \
    static struct { uint32_t magic, next; uint16_t version, mode; char name[8] __attribute__((nonstring)); \
                    uint32_t f[size + 1]; } var = { \
        OB_IRX_EXPORT_MAGIC, 0, (major) << 8 | (minor), 0, text, {0}}
#define FN(p) ((uint32_t)(uintptr_t)(p))

static int ret_zero(void) { return 0; }

/* stdio / sysmem Kprintf */
static int lib_vprintf(const char *format, va_list ap) {
    char text[256];
    int n = ob_vformat(text, sizeof(text), format, ap);
    ob_puts(text);
    return n;
}
static int lib_printf(const char *format, ...) {
    va_list ap; int n;
    va_start(ap, format); n = lib_vprintf(format, ap); va_end(ap);
    return n;
}
static int lib_putchar(int c) {
    char text[2] = {(char)c, 0};
    ob_puts(text);
    return c;
}
static int lib_puts(const char *s) { ob_puts(s); ob_puts("\n"); return 0; }

/* sysclib */
extern int ob_iop_setjmp(void *buffer);
extern void ob_iop_longjmp(void *buffer, int value);
static void *lib_memcpy(void *d, const void *s, uint32_t n) { return ob_copy(d, s, n); }
static void *lib_memmove(void *d, const void *s, uint32_t n) {
    uint8_t *dst = d; const uint8_t *src = s;
    if (dst < src) while (n--) *dst++ = *src++;
    else { dst += n; src += n; while (n--) *--dst = *--src; }
    return d;
}
static void *lib_memset(void *d, int c, uint32_t n) {
    uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d;
}
static void lib_bzero(void *d, uint32_t n) { ob_zero(d, n); }
static int lib_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = a, *y = b;
    for (; n; --n, ++x, ++y) if (*x != *y) return *x - *y;
    return 0;
}
static void *lib_memchr(const void *s, int c, uint32_t n) {
    const uint8_t *p = s;
    for (; n; --n, ++p) if (*p == (uint8_t)c) return (void *)p;
    return 0;
}
static uint32_t lib_strlen(const char *s) { uint32_t n = 0; while (s[n]) ++n; return n; }
/* BSD-style helpers: bcopy takes (source, destination). */
static int lib_bcmp(const void *a, const void *b, uint32_t n) { return lib_memcmp(a, b, n) != 0; }
static void lib_bcopy(const void *s, void *d, uint32_t n) { lib_memmove(d, s, n); }
static int in_set(char c, const char *set) { while (*set) if (*set++ == c) return 1; return 0; }
static uint32_t lib_strspn(const char *s, const char *set) {
    uint32_t n = 0; while (s[n] && in_set(s[n], set)) ++n; return n;
}
static uint32_t lib_strcspn(const char *s, const char *set) {
    uint32_t n = 0; while (s[n] && !in_set(s[n], set)) ++n; return n;
}
static char *lib_strpbrk(const char *s, const char *set) {
    for (; *s; ++s) if (in_set(*s, set)) return (char *)s;
    return 0;
}
static char *lib_strtok_r(char *s, const char *delim, char **saved) {
    char *token;
    if (!s) s = *saved;
    if (!s) return 0;
    s += lib_strspn(s, delim);
    if (!*s) { *saved = 0; return 0; }
    token = s;
    s += lib_strcspn(s, delim);
    if (*s) *s++ = 0; else s = 0;
    *saved = s;
    return token;
}
static char *strtok_state;
static char *lib_strtok(char *s, const char *delim) { return lib_strtok_r(s, delim, &strtok_state); }
/* Word-wise copy/fill; sizes are in bytes, rounded down to whole words. */
static void *lib_wmemcopy(uint32_t *d, const uint32_t *s, uint32_t n) {
    uint32_t *r = d; for (n /= 4; n; --n) *d++ = *s++; return r;
}
static void *lib_wmemset(uint32_t *d, uint32_t c, uint32_t n) {
    uint32_t *r = d; for (n /= 4; n; --n) *d++ = c; return r;
}
static char *lib_strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
static char *lib_strncpy(char *d, const char *s, uint32_t n) {
    uint32_t i = 0;
    for (; i < n && s[i]; ++i) d[i] = s[i];
    for (; i < n; ++i) d[i] = 0;
    return d;
}
static char *lib_strcat(char *d, const char *s) { lib_strcpy(d + lib_strlen(d), s); return d; }
static char *lib_strncat(char *d, const char *s, uint32_t n) {
    char *p = d + lib_strlen(d);
    while (n-- && *s) *p++ = *s++;
    *p = 0;
    return d;
}
static int lib_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (uint8_t)*a - (uint8_t)*b;
}
static int lib_strncmp(const char *a, const char *b, uint32_t n) {
    for (; n; --n, ++a, ++b) {
        if (*a != *b || !*a) return (uint8_t)*a - (uint8_t)*b;
    }
    return 0;
}
static char *lib_strchr(const char *s, int c) {
    for (;; ++s) { if (*s == (char)c) return (char *)s; if (!*s) return 0; }
}
static char *lib_strrchr(const char *s, int c) {
    const char *r = 0;
    for (;; ++s) { if (*s == (char)c) r = s; if (!*s) return (char *)r; }
}
static char *lib_strstr(const char *h, const char *n) {
    uint32_t len = lib_strlen(n);
    for (; *h; ++h) if (!lib_strncmp(h, n, len)) return (char *)h;
    return len ? 0 : (char *)h;
}
/* ctype classes: upper 0x01, lower 0x02, digit 0x04, space 0x08, punct 0x10, control 0x20,
   hex letter 0x40, blank 0x80. */
static unsigned char lib_look_ctype_table(char ch) {
    unsigned c = (unsigned char)ch;
    unsigned char r = 0;
    if (c >= 'A' && c <= 'Z') r |= 0x01;
    if (c >= 'a' && c <= 'z') r |= 0x02;
    if (c >= '0' && c <= '9') r |= 0x04;
    if (c == ' ' || (c >= 9 && c <= 13)) r |= 0x08;
    if (c < 32 || c == 127) r |= 0x20;
    if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) r |= 0x40;
    if (c == ' ') r |= 0x80;
    if (c > 32 && c < 127 && !(r & 0x07)) r |= 0x10;
    return r;
}
static char lib_toupper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static char lib_tolower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static unsigned long to_number(const char *s, char **end, int base, int *negative) {
    unsigned long v = 0;
    const char *start = s;
    int any = 0;
    while (*s == ' ' || (*s >= 9 && *s <= 13)) ++s;
    *negative = 0;
    if (*s == '-' || *s == '+') *negative = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0) base = s[0] == '0' ? 8 : 10;
    for (;; ++s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
        any = 1;
    }
    if (end) *end = (char *)(any ? s : start);
    return v;
}
static long lib_strtol(const char *s, char **end, int base) {
    int neg; unsigned long v = to_number(s, end, base, &neg);
    return neg ? -(long)v : (long)v;
}
static unsigned long lib_strtoul(const char *s, char **end, int base) {
    int neg; unsigned long v = to_number(s, end, base, &neg);
    return neg ? 0ul - v : v;
}
static int lib_vsprintf(char *out, const char *format, va_list ap) {
    return ob_vformat(out, 0x7fffffff, format, ap);
}
static int lib_sprintf(char *out, const char *format, ...) {
    va_list ap; int n;
    va_start(ap, format); n = lib_vsprintf(out, format, ap); va_end(ap);
    return n;
}

/* sysmem */
static void *lib_alloc(int type, uint32_t size, void *address) {
    void *p = ob_sysmem_alloc(type, size, (uint32_t)(uintptr_t)address);
    if (!p) { /* Modules rarely check for failure, so log it. */
        ob_puts("[openbios:iop] AllocSysMemory failed size="); ob_puthex(size);
        ob_puts(" free="); ob_puthex(ob_sysmem_free_size()); ob_puts("\n");
        ob_sysmem_dump();
    }
    return p;
}
static int lib_free(void *p) { return ob_sysmem_free(p); }
static uint32_t lib_query_memsize(void) { return 0x00200000u; }
static uint32_t lib_query_max_free(void) { return ob_sysmem_largest_free(); }
static uint32_t lib_query_total_free(void) { return ob_sysmem_free_size(); }
static void *lib_block_top(void *address) {
    uint32_t top = ob_sysmem_block((uint32_t)(uintptr_t)address, 0);
    return top ? (void *)(uintptr_t)top : (void *)-1;
}
static int lib_block_size(void *address) {
    uint32_t size = 0;
    return ob_sysmem_block((uint32_t)(uintptr_t)address, &size) ? (int)size : -1;
}

/* loadcore */
static void lib_flush_dcache(void) {} /* The IOP has no data cache. */
static int lib_register_library(struct ob_irx_exports *e) {
    int r = ob_iop_register_library(e);
    return r == KE_OK ? 0 : r;
}
static int lib_release_library(struct ob_irx_exports *e) { return ob_iop_release_library(e); }
static int lib_reboot_mode(void *e, int mode) { (void)e; (void)mode; return 0; }
/* Boot mode records (iop_bootmode_t: u16 value, u8 id, u8 len, then len
   data words), packed into one 16-word area. */
static uint32_t boot_modes[16];
static unsigned boot_modes_used;
static int *lib_query_boot_mode(int id) {
    unsigned i = 0;
    while (i < boot_modes_used) {
        uint32_t header = boot_modes[i];
        if ((int)((header >> 16) & 0xff) == id) return (int *)&boot_modes[i];
        i += 1 + (header >> 24);
    }
    return 0;
}
static void lib_register_boot_mode(const uint32_t *record) {
    unsigned words = 1 + (record[0] >> 24), i;
    if (boot_modes_used + words > 16) return;
    for (i = 0; i < words; ++i) boot_modes[boot_modes_used + i] = record[i];
    boot_modes_used += words;
}

LIBRARY(stdio, "stdio", 1, 3, 8);
LIBRARY(sysclib, "sysclib", 1, 4, 44);
LIBRARY(sysmem, "sysmem", 1, 2, 16);
LIBRARY(loadcore, "loadcore", 1, 3, 28);

void ob_iop_libraries_init(void) {
    unsigned i;
    for (i = 0; i < 4; ++i) {
        stdio.f[i] = sysclib.f[i] = sysmem.f[i] = loadcore.f[i] = FN(ret_zero);
    }
    stdio.f[4] = FN(lib_printf);
    stdio.f[6] = FN(lib_putchar);
    stdio.f[7] = FN(lib_puts);
    sysclib.f[4] = FN(ob_iop_setjmp);
    sysclib.f[5] = FN(ob_iop_longjmp);
    sysclib.f[6] = FN(lib_toupper);
    sysclib.f[7] = FN(lib_tolower);
    sysclib.f[8] = FN(lib_look_ctype_table);
    sysclib.f[10] = FN(lib_memchr);
    sysclib.f[11] = FN(lib_memcmp);
    sysclib.f[12] = FN(lib_memcpy);
    sysclib.f[13] = FN(lib_memmove);
    sysclib.f[14] = FN(lib_memset);
    sysclib.f[15] = FN(lib_bcmp);
    sysclib.f[16] = FN(lib_bcopy);
    sysclib.f[17] = FN(lib_bzero);
    sysclib.f[19] = FN(lib_sprintf);
    sysclib.f[20] = FN(lib_strcat);
    sysclib.f[21] = FN(lib_strchr);
    sysclib.f[22] = FN(lib_strcmp);
    sysclib.f[23] = FN(lib_strcpy);
    sysclib.f[24] = FN(lib_strcspn);
    sysclib.f[25] = FN(lib_strchr);  /* index */
    sysclib.f[26] = FN(lib_strrchr); /* rindex */
    sysclib.f[27] = FN(lib_strlen);
    sysclib.f[28] = FN(lib_strncat);
    sysclib.f[29] = FN(lib_strncmp);
    sysclib.f[30] = FN(lib_strncpy);
    sysclib.f[31] = FN(lib_strpbrk);
    sysclib.f[32] = FN(lib_strrchr);
    sysclib.f[33] = FN(lib_strspn);
    sysclib.f[35] = FN(lib_strtok);
    sysclib.f[34] = FN(lib_strstr);
    sysclib.f[36] = FN(lib_strtol);
    sysclib.f[38] = FN(lib_strtoul);
    sysclib.f[40] = FN(lib_wmemcopy);
    sysclib.f[41] = FN(lib_wmemset);
    sysclib.f[42] = FN(lib_vsprintf);
    sysclib.f[43] = FN(lib_strtok_r);
    sysmem.f[4] = FN(lib_alloc);
    sysmem.f[5] = FN(lib_free);
    sysmem.f[6] = FN(lib_query_memsize);
    sysmem.f[7] = FN(lib_query_max_free);
    sysmem.f[8] = FN(lib_query_total_free);
    sysmem.f[9] = FN(lib_block_top);
    sysmem.f[10] = FN(lib_block_size);
    sysmem.f[14] = FN(lib_printf); /* Kprintf */
    loadcore.f[4] = FN(lib_flush_dcache); /* FlushIcache */
    loadcore.f[5] = FN(lib_flush_dcache);
    loadcore.f[12] = FN(lib_query_boot_mode);
    loadcore.f[13] = FN(lib_register_boot_mode);
    boot_modes_used = 0;
    loadcore.f[6] = FN(lib_register_library);
    loadcore.f[7] = FN(lib_release_library);
    loadcore.f[27] = FN(lib_reboot_mode);
    ob_iop_register_builtin((void *)&stdio, 8);
    ob_iop_register_builtin((void *)&sysclib, 44);
    ob_iop_register_builtin((void *)&sysmem, 16);
    ob_iop_register_builtin((void *)&loadcore, 28);
}
