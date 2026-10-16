/* SPDX-License-Identifier: MIT */
#include "openbios/format.h"

struct sink { char *out; size_t size, len; };
static void emit(struct sink *s, char c) {
    if (s->size && s->len + 1 < s->size) s->out[s->len] = c;
    ++s->len;
}
static void pad(struct sink *s, char c, int n) { while (n-- > 0) emit(s, c); }

int ob_vformat(char *out, size_t size, const char *f, va_list ap) {
    struct sink s = {out, size, 0};
    while (*f) {
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, prec = -1;
        char digits[24], sign = 0, conv;
        const char *text = digits;
        int n = 0;
        uint32_t value;
        unsigned base = 10;
        if (*f != '%') { emit(&s, *f++); continue; }
        ++f;
        for (;; ++f) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '+') plus = 1;
            else if (*f == ' ') space = 1;
            else if (*f == '#') alt = 1;
            else break;
        }
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } ++f; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            ++f; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); ++f; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        while (*f == 'h' || *f == 'l' || *f == 'z' || *f == 't') ++f;
        conv = *f ? *f++ : 0;
        switch (conv) {
        case 'd': case 'i': {
            int32_t v = va_arg(ap, int32_t);
            if (v < 0) { sign = '-'; value = 0u - (uint32_t)v; }
            else { value = (uint32_t)v; sign = plus ? '+' : space ? ' ' : 0; }
            goto number;
        }
        case 'u': value = va_arg(ap, uint32_t); goto number;
        case 'o': value = va_arg(ap, uint32_t); base = 8; goto number;
        case 'p': value = (uint32_t)(uintptr_t)va_arg(ap, void *); base = 16; alt = 1; goto number;
        case 'x': case 'X': value = va_arg(ap, uint32_t); base = 16; goto number;
        number: {
            const char *set = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[24];
            int k = 0, prefix = (alt && base == 16 && value) ? 2 : 0, zeros;
            do { tmp[k++] = set[value % base]; value /= base; } while (value);
            if (prec == 0 && k == 1 && tmp[0] == '0') k = 0;
            zeros = prec > k ? prec - k : 0;
            if (prec < 0 && zero && !left) {
                int used = k + (sign != 0) + prefix;
                if (width > used) zeros = width - used;
            }
            n = k + zeros + (sign != 0) + prefix;
            if (!left) pad(&s, ' ', width - n);
            if (sign) emit(&s, sign);
            if (prefix) { emit(&s, '0'); emit(&s, conv == 'X' ? 'X' : 'x'); }
            pad(&s, '0', zeros);
            while (k) emit(&s, tmp[--k]);
            if (left) pad(&s, ' ', width - n);
            continue;
        }
        case 'c': digits[0] = (char)va_arg(ap, int); n = 1; break;
        case 's':
            text = va_arg(ap, const char *);
            if (!text) text = "(null)";
            while (text[n] && (prec < 0 || n < prec)) ++n;
            break;
        case '%': digits[0] = '%'; n = 1; break;
        default: /* Unknown conversion: print it verbatim. */
            emit(&s, '%');
            if (conv) emit(&s, conv);
            continue;
        }
        if (!left) pad(&s, ' ', width - n);
        { int k; for (k = 0; k < n; ++k) emit(&s, text[k]); }
        if (left) pad(&s, ' ', width - n);
    }
    if (size) out[s.len < size ? s.len : size - 1] = 0;
    return (int)s.len;
}
int ob_format(char *out, size_t size, const char *format, ...) {
    va_list ap;
    int n;
    va_start(ap, format);
    n = ob_vformat(out, size, format, ap);
    va_end(ap);
    return n;
}
