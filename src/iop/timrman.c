/* SPDX-License-Identifier: MIT */
/* timrman: hardware timer allocation for modules. Timer IDs are encoded
   as ((index + 1) << 28) | (counter address >> 4). TIMER5 drives the
   kernel tick and is never handed out. */
#include "openbios/iop.h"

#define KE_ILLEGAL_CONTEXT (-100)
#define KE_NO_TIMER (-150)
#define KE_ILLEGAL_TIMERID (-151)
#define KE_ILLEGAL_SOURCE (-152)
#define KE_ILLEGAL_PRESCALE (-153)
#define KE_TIMER_BUSY (-154)
#define KE_TIMER_NOT_SETUP (-155)
#define KE_TIMER_NOT_INUSE (-156)
#define KE_ILLEGAL_MODE (-405)
#define TIMERS 6

int ob_iop_register_builtin(struct ob_irx_exports *exports, uint32_t count);

typedef uint32_t (*timer_cb)(void *);
static struct timer {
    uint32_t addr; uint8_t sources, size; uint16_t max_prescale; uint8_t irq;
    int users, has_handler;
    uint32_t mode, config, timeup_flags, overflow_flags, compare;
    timer_cb timeup, overflow;
    void *timeup_arg, *overflow_arg;
    uint32_t timeup_gp, overflow_gp;
} timers[TIMERS] = {
    {0xbf801100u, 0xb, 16, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xbf801110u, 0xd, 16, 1, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xbf801120u, 0x1, 16, 8, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xbf801480u, 0x5, 32, 1, 14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xbf801490u, 0x1, 32, 256, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xbf8014a0u, 0x1, 32, 256, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, /* kernel tick */
};
/* Allocation preference order. */
static const int order[TIMERS] = {2, 5, 4, 3, 0, 1};

static uint32_t irq_lock(void) {
    uint32_t s; __asm__ volatile ("mfc0 %0, $12\n\tnop" : "=r"(s));
    __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s & ~1u) : "memory");
    return s;
}
static void irq_unlock(uint32_t s) { __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s) : "memory"); }
static uint32_t current_gp(void) { uint32_t g; __asm__ volatile ("move %0, $28" : "=r"(g)); return g; }
static struct timer *lookup(int id) {
    uint32_t index = ((uint32_t)id >> 28) - 1;
    return index < TIMERS ? &timers[index] : 0;
}
static volatile uint32_t *reg(struct timer *t, unsigned offset) {
    return (volatile uint32_t *)(uintptr_t)(t->addr + offset);
}
static void write_reg(struct timer *t, unsigned offset, uint32_t v) {
    if (t->size == 32) *reg(t, offset) = v;
    else *(volatile uint16_t *)reg(t, offset) = (uint16_t)v;
}
static uint32_t read_reg(struct timer *t, unsigned offset) {
    return t->size == 32 ? *reg(t, offset) : *(volatile uint16_t *)reg(t, offset);
}

static int AllocHardTimer(int source, int size, int prescale) {
    uint32_t s;
    int i;
    if (ob_intr_context()) return KE_ILLEGAL_CONTEXT;
    s = irq_lock();
    for (i = 0; i < TIMERS; ++i) {
        struct timer *t = &timers[order[i]];
        if (t->users || !(t->sources & source) || t->size != size || t->max_prescale < prescale)
            continue;
        t->users = 1;
        t->mode = t->config = t->timeup_flags = t->overflow_flags = 0;
        t->timeup = t->overflow = 0;
        write_reg(t, 4, 0);
        ob_intr_enable(t->irq, 0);
        irq_unlock(s);
        return (int)((uint32_t)(order[i] + 1) << 28 | (t->addr >> 4 & 0x0fffffffu));
    }
    irq_unlock(s);
    return KE_NO_TIMER;
}
/* Share a timer another module allocated and configured: same source, size and
   mode bits under modemask. */
static int ReferHardTimer(int source, int size, int mode, int modemask) {
    uint32_t s;
    int i;
    if (ob_intr_context()) return KE_ILLEGAL_CONTEXT;
    s = irq_lock();
    for (i = 0; i < TIMERS; ++i) {
        struct timer *t = &timers[i];
        if (!t->users || !t->mode || !(t->sources & source) || t->size != size ||
            (t->mode & (uint32_t)modemask) != (uint32_t)mode) continue;
        ++t->users;
        irq_unlock(s);
        return (int)((uint32_t)(i + 1) << 28 | (t->addr >> 4 & 0x0fffffffu));
    }
    irq_unlock(s);
    return KE_NO_TIMER;
}
static int FreeHardTimer(int id) {
    struct timer *t = lookup(id);
    uint32_t s;
    if (ob_intr_context()) return KE_ILLEGAL_CONTEXT;
    if (!t || t == &timers[5] || !t->users) return KE_ILLEGAL_TIMERID;
    s = irq_lock();
    if (t->users > 1) { --t->users; irq_unlock(s); return 0; } /* still referenced */
    t->users = 0;
    t->mode = 0;
    write_reg(t, 4, 0);
    ob_intr_enable(t->irq, 0);
    if (t->has_handler) ob_intr_release(t->irq);
    t->has_handler = 0;
    t->timeup = t->overflow = 0;
    irq_unlock(s);
    return 0;
}
static void SetTimerMode(int id, int mode) {
    struct timer *t = lookup(id);
    if (!t) return;
    t->mode = (uint32_t)mode;
    write_reg(t, 4, (uint32_t)mode);
}
static uint32_t GetTimerMode(int id) { struct timer *t = lookup(id); return t ? t->mode : (uint32_t)KE_ILLEGAL_TIMERID; }
static uint32_t GetTimerStatus(int id) { struct timer *t = lookup(id); return t ? read_reg(t, 4) : 0; }
static void SetTimerCounter(int id, uint32_t v) { struct timer *t = lookup(id); if (t) write_reg(t, 0, v); }
static uint32_t GetTimerCounter(int id) { struct timer *t = lookup(id); return t ? read_reg(t, 0) : 0; }
static void SetTimerCompare(int id, uint32_t v) { struct timer *t = lookup(id); if (t) write_reg(t, 8, v); }
static uint32_t GetTimerCompare(int id) { struct timer *t = lookup(id); return t ? read_reg(t, 8) : 0; }
static int GetHardTimerIntrCode(int id) { struct timer *t = lookup(id); return t ? t->irq : KE_ILLEGAL_TIMERID; }

static uint32_t call_cb(timer_cb cb, void *arg, uint32_t gp) {
    uint32_t saved, r;
    __asm__ volatile ("move %0, $28" : "=r"(saved));
    if (gp) __asm__ volatile ("move $28, %0" :: "r"(gp));
    r = cb(arg);
    __asm__ volatile ("move $28, %0" :: "r"(saved));
    return r;
}
static int timer_irq(void *arg) {
    struct timer *t = arg;
    uint32_t status = read_reg(t, 4), stop = 0, r;
    if ((status & (1u << 12)) && t->overflow) {
        if (!call_cb(t->overflow, t->overflow_arg, t->overflow_gp)) stop |= 1u << 5;
    }
    if ((status & (1u << 11)) && t->timeup) {
        r = call_cb(t->timeup, t->timeup_arg, t->timeup_gp);
        if (!r) stop |= 1u << 4;
        else write_reg(t, 8, r); /* Non-zero result is the next compare value. */
    }
    if (stop || !t->mode) {
        t->mode = 0;
        write_reg(t, 4, 0);
        return 0;
    }
    return 1;
}
static int SetTimerHandler(int id, uint32_t compare, timer_cb handler, void *arg) {
    struct timer *t = lookup(id);
    uint32_t s;
    if (!t) return KE_ILLEGAL_TIMERID;
    s = irq_lock();
    if (t->mode) { irq_unlock(s); return KE_TIMER_NOT_INUSE; }
    t->compare = compare;
    t->timeup = handler;
    t->timeup_arg = arg;
    t->timeup_gp = current_gp();
    t->timeup_flags = handler ? (1u << 3) | (1u << 4) | (1u << 6) : 0;
    irq_unlock(s);
    return 0;
}
static int SetOverflowHandler(int id, timer_cb handler, void *arg) {
    struct timer *t = lookup(id);
    uint32_t s;
    if (!t) return KE_ILLEGAL_TIMERID;
    s = irq_lock();
    if (t->mode) { irq_unlock(s); return KE_TIMER_NOT_INUSE; }
    t->overflow = handler;
    t->overflow_arg = arg;
    t->overflow_gp = current_gp();
    t->overflow_flags = handler ? (1u << 5) | (1u << 6) : 0;
    irq_unlock(s);
    return 0;
}
static int SetupHardTimer(int id, int source, int mode, int prescale) {
    struct timer *t = lookup(id);
    uint32_t s, config;
    unsigned index;
    if (ob_intr_context()) return KE_ILLEGAL_CONTEXT;
    if (!t) return KE_ILLEGAL_TIMERID;
    index = (unsigned)(t - timers);
    if (mode != 0 && mode != 1 && mode != 3 && mode != 5 && mode != 7) return KE_ILLEGAL_MODE;
    config = (uint32_t)mode | 0x80000000u; /* Bit 31 marks "set up". */
    if (source == 2 || source == 4) config |= 1u << 8;
    else if (source != 1) return KE_ILLEGAL_SOURCE;
    switch (prescale) {
    case 1: break;
    case 8: config |= index >= 3 ? 0x2000u : 0x200u; break;
    case 16: config |= 0x4000u; break;
    case 256: config |= 0x6000u; break;
    default: return KE_ILLEGAL_PRESCALE;
    }
    s = irq_lock();
    if (t->mode) { irq_unlock(s); return KE_TIMER_BUSY; }
    if (!t->has_handler) {
        ob_intr_enable(t->irq, 0);
        if (ob_intr_register(t->irq, timer_irq, t) < 0) { irq_unlock(s); return KE_TIMER_BUSY; }
        t->has_handler = 1;
    }
    t->config = config;
    irq_unlock(s);
    return 0;
}
static int StartHardTimer(int id) {
    struct timer *t = lookup(id);
    uint32_t s;
    if (!t) return KE_ILLEGAL_TIMERID;
    s = irq_lock();
    if (t->mode) { irq_unlock(s); return KE_TIMER_BUSY; }
    if (!t->config) { irq_unlock(s); return KE_TIMER_NOT_SETUP; }
    write_reg(t, 4, 0);
    if (t->timeup_flags) write_reg(t, 8, t->compare);
    t->mode = (t->config & 0x7fffffffu) | t->timeup_flags | t->overflow_flags;
    if (!t->mode) t->mode = 0x80000000u; /* Running with no flags still counts as busy. */
    irq_unlock(s);
    write_reg(t, 4, t->mode & 0xffffu);
    if (t->mode & 0x30) ob_intr_enable(t->irq, 1);
    return 0;
}
static int StopHardTimer(int id) {
    struct timer *t = lookup(id);
    uint32_t s;
    if (!t) return KE_ILLEGAL_TIMERID;
    s = irq_lock();
    if (!t->mode) { irq_unlock(s); return KE_TIMER_NOT_INUSE; }
    if (t->mode & 0x30) ob_intr_enable(t->irq, 0);
    t->mode = 0;
    write_reg(t, 4, 0);
    irq_unlock(s);
    return 0;
}

static int ret_zero(void) { return 0; }
static int ret_neg(void) { return -1; }
static struct { uint32_t magic, next; uint16_t version, mode; char name[8] __attribute__((nonstring));
                uint32_t f[29]; } timrman = {OB_IRX_EXPORT_MAGIC, 0, 0x0103, 0, "timrman", {0}};
#define FN(p) ((uint32_t)(uintptr_t)(p))
void ob_iop_timrman_init(void) {
    int i;
    for (i = 0; i < 5; ++i) {
        timers[i].users = timers[i].has_handler = 0;
        timers[i].mode = timers[i].config = 0;
        timers[i].timeup = timers[i].overflow = 0;
    }
    timrman.f[0] = timrman.f[1] = timrman.f[2] = timrman.f[3] = FN(ret_zero);
    timrman.f[4] = FN(AllocHardTimer);   timrman.f[5] = FN(ReferHardTimer);
    timrman.f[6] = FN(FreeHardTimer);
    timrman.f[7] = FN(SetTimerMode);     timrman.f[8] = FN(GetTimerStatus);
    timrman.f[9] = FN(SetTimerCounter);  timrman.f[10] = FN(GetTimerCounter);
    timrman.f[11] = FN(SetTimerCompare); timrman.f[12] = FN(GetTimerCompare);
    timrman.f[16] = FN(GetHardTimerIntrCode); timrman.f[17] = FN(GetTimerMode);
    timrman.f[19] = FN(ret_neg);
    timrman.f[20] = FN(SetTimerHandler); timrman.f[21] = FN(SetOverflowHandler);
    timrman.f[22] = FN(SetupHardTimer);  timrman.f[23] = FN(StartHardTimer);
    timrman.f[24] = FN(StopHardTimer);
    timrman.f[25] = timrman.f[26] = timrman.f[27] = FN(ret_neg);
    ob_iop_register_builtin((void *)&timrman, 28);
}
