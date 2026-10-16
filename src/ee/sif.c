/* SPDX-License-Identifier: MIT */
#include "openbios/ee.h"
#include "openbios/platform.h"
#include "openbios/sifdma.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SIF0_CHCR REG32(0xb000c000u)
#define SIF0_QWC REG32(0xb000c020u)
#define SIF1_CHCR REG32(0xb000c400u)
#define SIF1_MADR REG32(0xb000c410u)
#define SIF1_QWC REG32(0xb000c420u)
#define SIF1_TADR REG32(0xb000c430u)
#define DMAC_STAT REG32(0xb000e010u)
#define SIF1_STAT (1u << 6)
#define SIF1_MASK (1u << 22)
static uint32_t system_registers[3];
static struct ob_sifdma_queue dma_queue;
static int dma_mask_owned, dma_logged;

void ob_ee_sif_init(void) {
    ob_zero(system_registers, sizeof(system_registers));
    ob_sifdma_init(&dma_queue, (uint32_t)(uintptr_t)&dma_queue & 0x1fffffffu);
    dma_mask_owned = 0;
    /* Enable the DMAC globally; channel masks remain under driver control. */
    REG32(0xb000e000u) |= 1;
    /* Programs expect MSFLAG SIFINIT set by the EE side. */
    REG32(0xb000f220u) = 0x10000;
}
int32_t ob_ee_sif_get_reg(uint32_t id) {
    if (id >= 0x80000000u && id < 0x80000003u)
        return (int32_t)system_registers[id - 0x80000000u];
    if (id >= 1 && id <= 4) return (int32_t)REG32(0xb000f200u + (id - 1) * 16);
    return 0; /* Unknown IDs read as zero. */
}
int32_t ob_ee_sif_set_reg(uint32_t id, uint32_t value) {
    if (id >= 0x80000000u && id < 0x80000003u)
        system_registers[id - 0x80000000u] = value;
    else if (id >= 1 && id <= 4) REG32(0xb000f200u + (id - 1) * 16) = value;
    return 0; /* Not the previous value. */
}
void ob_ee_sif_set_dchain(void) {
    /* Destination-chain tags come from the IOP FIFO, not EE TADR. Leave an
       armed receiver alone so a partial payload is not lost. */
    if (SIF0_CHCR & 0x100) return;
    SIF0_QWC = 0;
    __asm__ volatile ("sync" ::: "memory");
    SIF0_CHCR = 0x184; /* CHAIN | TIE | STR, IOP -> EE. */
}

/* SifStopDma: stop the EE SIF0 receiver (channel 5), as before an IOP reset. */
void ob_ee_sif_stop_dma(void) {
    if (SIF0_CHCR & 0x100) SIF0_CHCR = SIF0_CHCR & ~0x100u;
    __asm__ volatile ("sync" ::: "memory");
}
static void dma_start(void) {
    /* Clear a stale completion before starting; status is write-one-to-clear. */
    DMAC_STAT = SIF1_STAT;
    if (!(DMAC_STAT & SIF1_MASK)) {
        DMAC_STAT = SIF1_MASK; /* Upper bits toggle; enable completion interrupt. */
        dma_mask_owned = 1;
    }
    SIF1_QWC = 0;
    SIF1_MADR = 0;
    SIF1_TADR = ob_sifdma_active_tags(&dma_queue);
    __asm__ volatile ("sync" ::: "memory");
    SIF1_CHCR = 0x105; /* Source chain, from memory, start. No TTE or TIE. */
}
/* Retire the active chain only after the channel has stopped by itself. */
static void dma_update(void) {
    if (dma_queue.active < 0 || (SIF1_CHCR & 0x100)) return;
    if (ob_sifdma_complete(&dma_queue)) dma_start();
    else if (dma_mask_owned) {
        if (DMAC_STAT & SIF1_MASK) DMAC_STAT = SIF1_MASK;
        dma_mask_owned = 0;
    }
}
void ob_ee_sif_dma_irq(void) { dma_update(); }
void ob_ee_sif_dma_mask_changed(void) { dma_mask_owned = 0; }

uint32_t ob_ee_sif_set_dma(uint32_t descriptors, int32_t count) {
    struct ob_sifdma_xfer x[OB_SIFDMA_MAX];
    const uint32_t *in;
    uint32_t i, id;
    int start, result = OB_EINVAL;
    /* Zero or negative counts still return an ID and move nothing. */
    if (count < 0) count = 0;
    if ((uint32_t)count > OB_SIFDMA_MAX || (descriptors & 3)) goto rejected;
    in = count ? ob_ee_user_pointer(descriptors, (uint32_t)count * 16) : 0;
    if (count && !in) goto rejected;
    for (i = 0; i < (uint32_t)count; ++i) {
        uint32_t src = in[i * 4], bytes;
        x[i].dest = in[i * 4 + 1] & 0x1fffffffu;
        x[i].size = (int32_t)in[i * 4 + 2];
        x[i].attr = in[i * 4 + 3];
        bytes = x[i].size > 0 ? ((uint32_t)x[i].size + 15) & ~15u : 16;
        /* The DMAC ignores the low four MADR bits. Scratchpad sources would
           need the SPR flag and are not supported. */
        src &= ~15u;
        if ((src >> 28) == 7 || !ob_ee_user_pointer(src, bytes)) goto rejected;
        x[i].src = (uint32_t)(uintptr_t)ob_ee_user_pointer(src, bytes) & 0x1fffffffu;
    }
    result = ob_sifdma_check(x, (uint32_t)count);
    if (result) goto rejected;
    if (count && !dma_logged) {
        dma_logged = 1;
        ob_puts("[openbios:ee] SifSetDma first src="); ob_puthex(in[0]);
        ob_puts(" dest="); ob_puthex(in[1]); ob_puts(" size="); ob_puthex(in[2]);
        ob_puts(" attr="); ob_puthex(in[3]); ob_puts(" count="); ob_puthex((uint32_t)count);
        ob_puts("\n");
    }
#ifdef OB_TRACE_SYSCALLS
    for (i = 0; i < (uint32_t)count; ++i) {
        const uint32_t *w = (const uint32_t *)(uintptr_t)(x[i].src | 0xa0000000u);
        unsigned k;
        ob_puts("[openbios:trace] sifdma src="); ob_puthex(x[i].src);
        ob_puts(" dest="); ob_puthex(x[i].dest); ob_puts(" size="); ob_puthex((uint32_t)x[i].size);
        ob_puts(" attr="); ob_puthex(x[i].attr); ob_puts(" data=");
        for (k = 0; k < 16 && k * 4 < (unsigned)x[i].size; ++k) { ob_puthex(w[k]); ob_puts(" "); }
        ob_puts("\n");
    }
#endif
    dma_update();
    id = ob_sifdma_submit(&dma_queue, x, (uint32_t)count, &start);
    if (start) dma_start();
    return id;
rejected:
    ob_puts("[openbios:ee] SifSetDma rejected descriptors="); ob_puthex(descriptors);
    ob_puts(" count="); ob_puthex((uint32_t)count);
    ob_puts(" error="); ob_puthex((uint32_t)result); ob_puts("\n");
    return 0;
}
int32_t ob_ee_sif_dma_stat(uint32_t id) {
    dma_update();
    return ob_sifdma_status(&dma_queue, id);
}
