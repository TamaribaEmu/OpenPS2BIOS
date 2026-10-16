/* SPDX-License-Identifier: MIT */
/* dmacman: IOP DMA channel register access. Channels 0-6 use the original
   controller (0x1f801080, DPCR nibbles), 7-12 the second one (0x1f801500,
   DPCR2 nibbles). Each nibble holds a 3-bit priority and an enable bit. */
#include "openbios/iop.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define DPCR REG32(0xbf8010f0u)
#define DPCR2 REG32(0xbf801570u)
#define DICR REG32(0xbf8010f4u)
#define DICR2 REG32(0xbf801574u)
#define CHANNELS 13

int ob_iop_register_builtin(struct ob_irx_exports *exports, uint32_t count);

static volatile uint32_t *channel_reg(uint32_t ch, unsigned offset) {
    uint32_t base = ch < 7 ? 0xbf801080u + ch * 0x10u : 0xbf801500u + (ch - 7) * 0x10u;
    return (volatile uint32_t *)(uintptr_t)(base + offset);
}
static uint32_t irq_lock(void) {
    uint32_t s; __asm__ volatile ("mfc0 %0, $12\n\tnop" : "=r"(s));
    __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s & ~1u) : "memory");
    return s;
}
static void irq_unlock(uint32_t s) { __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s) : "memory"); }

static void set_madr(uint32_t ch, uint32_t v) { if (ch < CHANNELS) *channel_reg(ch, 0) = v; }
static uint32_t get_madr(uint32_t ch) { return ch < CHANNELS ? *channel_reg(ch, 0) : 0; }
static void set_bcr(uint32_t ch, uint32_t v) { if (ch < CHANNELS) *channel_reg(ch, 4) = v; }
static uint32_t get_bcr(uint32_t ch) { return ch < CHANNELS ? *channel_reg(ch, 4) : 0; }
static void set_chcr(uint32_t ch, uint32_t v) { if (ch < CHANNELS) *channel_reg(ch, 8) = v; }
static uint32_t get_chcr(uint32_t ch) { return ch < CHANNELS ? *channel_reg(ch, 8) : 0; }
static void set_tadr(uint32_t ch, uint32_t v) { if (ch < CHANNELS) *channel_reg(ch, 12) = v; }
static uint32_t get_tadr(uint32_t ch) { return ch < CHANNELS ? *channel_reg(ch, 12) : 0; }
static void set_dpcr(uint32_t v) { DPCR = v; }
static uint32_t get_dpcr(void) { return DPCR; }
static void set_dpcr2(uint32_t v) { DPCR2 = v; }
static uint32_t get_dpcr2(void) { return DPCR2; }
static void set_dicr(uint32_t v) { DICR = v; }
static uint32_t get_dicr(void) { return DICR; }
static void set_dicr2(uint32_t v) { DICR2 = v; }
static uint32_t get_dicr2(void) { return DICR2; }

/* Slice (block) mode: size words per block, count blocks; dir 1 = from RAM. */
static int sceSetSliceDMA(uint32_t ch, void *addr, uint32_t size, uint32_t count, int dir) {
    if (ch >= CHANNELS || ch == 6) return 0;
    set_madr(ch, (uint32_t)(uintptr_t)addr & 0xffffffu);
    set_bcr(ch, (size & 0xffffu) | (count << 16));
    set_chcr(ch, (uint32_t)(dir & 1) | 0x200u | (dir == 0 ? 0x40000000u : 0));
    return 1;
}
static void sceStartDMA(uint32_t ch) { if (ch < CHANNELS) set_chcr(ch, get_chcr(ch) | 0x01000000u); }
static void dpcr_field(uint32_t ch, uint32_t mask, uint32_t value) {
    uint32_t s = irq_lock();
    if (ch < 7) DPCR = (DPCR & ~(mask << (ch * 4))) | (value << (ch * 4));
    else if (ch < CHANNELS) DPCR2 = (DPCR2 & ~(mask << ((ch - 7) * 4))) | (value << ((ch - 7) * 4));
    irq_unlock(s);
}
static void sceSetDMAPriority(uint32_t ch, uint32_t priority) { dpcr_field(ch, 7, priority & 7); }
static void sceEnableDMAChannel(uint32_t ch) {
    uint32_t s = irq_lock();
    if (ch < 7) DPCR |= 8u << (ch * 4);
    else if (ch < CHANNELS) DPCR2 |= 8u << ((ch - 7) * 4);
    irq_unlock(s);
}
static void sceDisableDMAChannel(uint32_t ch) { dpcr_field(ch, 8, 0); }

static int ret_zero(void) { return 0; }
static struct { uint32_t magic, next; uint16_t version, mode; char name[8] __attribute__((nonstring));
                uint32_t f[37]; } dmacman = {OB_IRX_EXPORT_MAGIC, 0, 0x0102, 0, "dmacman", {0}};
#define FN(p) ((uint32_t)(uintptr_t)(p))
void ob_iop_dmacman_init(void) {
    dmacman.f[0] = dmacman.f[1] = dmacman.f[2] = dmacman.f[3] = FN(ret_zero);
    dmacman.f[4] = FN(set_madr);  dmacman.f[5] = FN(get_madr);
    dmacman.f[6] = FN(set_bcr);   dmacman.f[7] = FN(get_bcr);
    dmacman.f[8] = FN(set_chcr);  dmacman.f[9] = FN(get_chcr);
    dmacman.f[10] = FN(set_tadr); dmacman.f[11] = FN(get_tadr);
    dmacman.f[14] = FN(set_dpcr); dmacman.f[15] = FN(get_dpcr);
    dmacman.f[16] = FN(set_dpcr2); dmacman.f[17] = FN(get_dpcr2);
    dmacman.f[20] = FN(set_dicr); dmacman.f[21] = FN(get_dicr);
    dmacman.f[22] = FN(set_dicr2); dmacman.f[23] = FN(get_dicr2);
    dmacman.f[28] = FN(sceSetSliceDMA);
    dmacman.f[32] = FN(sceStartDMA);
    dmacman.f[33] = FN(sceSetDMAPriority);
    dmacman.f[34] = FN(sceEnableDMAChannel);
    dmacman.f[35] = FN(sceDisableDMAChannel);
    ob_iop_register_builtin((void *)&dmacman, 36);
}
