/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_PLATFORM_H
#define OPENBIOS_PLATFORM_H
#include "base.h"
void ob_puts(const char *text);
void ob_puthex(uint32_t value);
void ob_halt(void) __attribute__((noreturn));
/* IOP console text is also mirrored into a ring in IOP RAM, which the EE
   kernel drains to its SIO. */
#define OB_IOP_LOG_IOP 0xa0083000u
#define OB_IOP_LOG_EE 0xbc083000u
#define OB_IOP_LOG_MAGIC 0x4f424c47u
#define OB_IOP_LOG_DATA 4080u
struct ob_iop_log { uint32_t magic, head, tail, reserved; uint8_t data[OB_IOP_LOG_DATA]; };
void ob_iop_log_init(void);
void ob_ee_iop_log_drain(void);
#endif
