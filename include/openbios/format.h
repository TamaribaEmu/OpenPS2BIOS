/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_FORMAT_H
#define OPENBIOS_FORMAT_H
#include <stdarg.h>
#include "base.h"
/* printf-style formatting: flags "-0 +#", width/precision (also '*'), length
   "hh h l ll" (accepted; arguments are 32-bit on the IOP), conversions
   d i u x X o c s p %. Output is truncated to size-1 and NUL-terminated when
   size > 0. Returns the length the full output would have. */
int ob_vformat(char *out, size_t size, const char *format, va_list args);
int ob_format(char *out, size_t size, const char *format, ...);
#endif
