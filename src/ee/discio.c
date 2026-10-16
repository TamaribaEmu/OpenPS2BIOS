/* SPDX-License-Identifier: MIT */
/* EE side of the bootstrap disc mailbox (see openbios/disc.h): sector reads
   with read-ahead, and drive status. Used by the kernel and the boot screen. */
#include "openbios/disc.h"
#include "openbios/platform.h"

/* IOP wait limit in COP0 Count ticks (EE clock, 294.912 MHz); generous
   because the drive may still be spinning up. */
#define WAIT_CYCLES (294912000ull * 20u)
static uint32_t count_now(void) { uint32_t c; __asm__ volatile ("mfc0 %0, $9" : "=r"(c)); return c; }
static int wait_mailbox(volatile struct ob_disc_mailbox *m, int (*done)(volatile struct ob_disc_mailbox *)) {
    uint64_t waited = 0;
    uint32_t last = count_now();
    while (!done(m)) {
        uint32_t now = count_now();
        waited += now - last;
        last = now;
        if (waited > WAIT_CYCLES) return 0;
    }
    return 1;
}
static int has_magic(volatile struct ob_disc_mailbox *m) { return m->magic == OB_DISC_MAGIC; }
static int read_done(volatile struct ob_disc_mailbox *m) { return m->state == 2; }
static int status_done(volatile struct ob_disc_mailbox *m) { return m->state == 6; }

/* One request; returns its result (OB_EBUSY on timeout). */
static int request(uint32_t state, uint32_t lba, uint32_t detail,
                   int (*done)(volatile struct ob_disc_mailbox *), uint32_t *out) {
    volatile struct ob_disc_mailbox *m = (void *)OB_DISC_MAILBOX_EE;
    int ok;
    if (!wait_mailbox(m, has_magic) || (m->state != 0 && m->state != 2 && m->state != 6))
        return OB_EBUSY;
    m->lba = lba;
    m->detail = detail;
    m->result = OB_EBUSY;
    __asm__ volatile ("sync" ::: "memory");
    m->state = state;
    ok = wait_mailbox(m, done);
    __asm__ volatile ("sync" ::: "memory");
    if (out) *out = m->detail;
    if (!ok) return OB_EBUSY;
    ok = m->result;
    m->state = 0;
    return ok;
}

static uint32_t cache_lba, cache_count;
void ob_ee_disc_invalidate(void) { cache_count = 0; }

int ob_ee_disc_read(void *ctx, uint32_t lba, uint8_t out[2048]) {
    uint32_t detail = 0;
    int result;
    (void)ctx;
    if (lba - cache_lba < cache_count) {
        ob_copy(out, (const void *)(uintptr_t)(OB_DISC_BULK_EE + (lba - cache_lba) * 2048u), 2048);
        return OB_OK;
    }
    /* Read ahead into the bulk buffer; near the end of the disc the bulk
       read fails and a single-sector read follows. */
    cache_count = 0;
    result = request(1, lba, OB_DISC_BULK_SECTORS, read_done, &detail);
    if (result == OB_OK) {
        cache_lba = lba;
        cache_count = OB_DISC_BULK_SECTORS;
        ob_copy(out, (const void *)OB_DISC_BULK_EE, 2048);
        return OB_OK;
    }
    result = request(1, lba, 1, read_done, &detail);
    if (result) {
        ob_puts("[openbios:disc] read failed lba="); ob_puthex(lba);
        ob_puts(" detail="); ob_puthex(detail); ob_puts("\n");
        return result;
    }
    ob_copy(out, (const void *)OB_DISC_BUFFER_EE, 2048);
    return OB_OK;
}

int ob_ee_disc_status(void) {
    uint32_t detail = 0;
    int result = request(5, 0, 0, status_done, &detail);
    return result ? result : (int)(detail & 0xffffu);
}
