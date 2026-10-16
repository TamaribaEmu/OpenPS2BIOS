/* SPDX-License-Identifier: MIT */
#include "openbios/platform.h"

#ifdef OB_IOP
void ob_iop_log_init(void) {
    volatile struct ob_iop_log *log = (void *)OB_IOP_LOG_IOP;
    log->head = log->tail = 0;
    log->magic = OB_IOP_LOG_MAGIC;
}
void ob_puts(const char *text) {
    volatile uint8_t *out = (volatile uint8_t *)0xbf802080u;
    volatile struct ob_iop_log *log = (void *)OB_IOP_LOG_IOP;
    uint32_t head = log->head;
    int ring = log->magic == OB_IOP_LOG_MAGIC;
    for (; *text; ++text) {
        *out = (uint8_t)*text;
        if (ring && head - log->tail < OB_IOP_LOG_DATA) /* Drop text when full. */
            log->data[head++ % OB_IOP_LOG_DATA] = (uint8_t)*text;
    }
    __asm__ volatile ("" ::: "memory");
    if (ring) log->head = head;
}
#else
void ob_puts(const char *text) {
    volatile uint8_t *out = (volatile uint8_t *)0xb000f180u;
    while (*text) *out = (uint8_t)*text++;
}
/* Copy pending IOP console text to the EE SIO. */
void ob_ee_iop_log_drain(void) {
    volatile struct ob_iop_log *log = (void *)OB_IOP_LOG_EE;
    volatile uint8_t *out = (volatile uint8_t *)0xb000f180u;
    uint32_t tail, head;
    if (log->magic != OB_IOP_LOG_MAGIC) return;
    head = log->head;
    tail = log->tail;
    if (head - tail > OB_IOP_LOG_DATA) tail = head - OB_IOP_LOG_DATA;
    while (tail != head) *out = log->data[tail++ % OB_IOP_LOG_DATA];
    log->tail = tail;
}
#endif
void ob_puthex(uint32_t value) {
    static const char hex[] = "0123456789abcdef";
    char buffer[9];
    unsigned i;
    for (i = 0; i < 8; ++i) buffer[i] = hex[(value >> (28 - i * 4)) & 15];
    buffer[8] = 0;
    ob_puts(buffer);
}
void ob_halt(void) {
    for (;;) __asm__ volatile ("nop");
}
