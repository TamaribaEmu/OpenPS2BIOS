/* SPDX-License-Identifier: MIT */
/* IOP glue for the portable kernel core: exception dispatch, interrupt routing
   (INTC lines 0..31 and DMA channels as 0x20+), the system tick, and the
   thbase/thsemap/thevent/thfpool/thvpool/thmsgbx/intrman libraries exported to IRX modules. */
#include "openbios/iop.h"
#include "openbios/iopk.h"
#include "openbios/format.h"
#include "openbios/platform.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define I_STAT REG32(0xbf801070u)
#define I_MASK REG32(0xbf801074u)
#define I_CTRL REG32(0xbf801078u)
#define DICR REG32(0xbf8010f4u)
#define DICR2 REG32(0xbf801574u)
#define T5_COUNT REG32(0xbf8014a0u)
#define T5_MODE REG32(0xbf8014a4u)
#define T5_TARGET REG32(0xbf8014a8u)
#define TICK_CYCLES 36864u /* 1 ms at 36.864 MHz */
#define INTR_TIMER5 16
#define INTR_DMA 3

int ob_iop_register_builtin(struct ob_irx_exports *exports, uint32_t count);
extern void ob_iop_vector_install(void);
extern int32_t ob_iop_yield(void);
extern void ob_iop_thread_return(void);

static struct obk_kernel k;
static struct obk_context boot_context;
struct obk_context *ob_iop_context = &boot_context;
uint8_t ob_iop_kernel_stack[8192] __attribute__((aligned(16)));
__asm__(".globl ob_iop_kernel_stack_top\n.set ob_iop_kernel_stack_top, ob_iop_kernel_stack + 8192 - 16");
static volatile int in_interrupt;
static volatile uint32_t ticks;
static struct { int (*handler)(void *); void *arg; uint32_t gp; int mode; } handlers[64];

static uint32_t sr_read(void) { uint32_t v; __asm__ volatile ("mfc0 %0, $12\n\tnop" : "=r"(v)); return v; }
static void sr_write(uint32_t v) { __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(v) : "memory"); }
static uint32_t lock(void) { uint32_t s = sr_read(); sr_write(s & ~1u); return s; }
static void unlock(uint32_t s) { sr_write(s); }
static uint32_t current_gp(void) { uint32_t g; __asm__ volatile ("move %0, $28" : "=r"(g)); return g; }

/* Called with interrupts disabled: trap into the scheduler. The return value is
   the v0 delivered to this thread while it waited. */
static int32_t block_now(void) { return ob_iop_yield(); }
static void maybe_preempt(void) {
    if (!in_interrupt && obk_pick(&k) != k.current) ob_iop_yield();
}

uint64_t ob_iop_system_time(void) {
    uint32_t s = lock(), t = ticks, c = T5_COUNT;
    /* A pending target interrupt means the count already wrapped. */
    if ((I_STAT & (1u << INTR_TIMER5)) && c < TICK_CYCLES / 2) ++t;
    unlock(s);
    return (uint64_t)t * TICK_CYCLES + c;
}

#define KE_FOUND_HANDLER (-104)
#define KE_NOTFOUND_ALARM (-105) /* KE_NOTFOUND_HANDLER */
static void run_alarms(uint64_t now);
static uint32_t seen_irq[2];
#ifdef OB_TRACE_RPC
static uint32_t irq_counts[0x30];
#endif
static int call_handler(int irq) {
    int (*h)(void *) = handlers[irq].handler;
    uint32_t saved;
    int r;
    if (!h) return 0;
#ifdef OB_TRACE_RPC
    if (irq < 0x30) ++irq_counts[irq];
#endif
    if (handlers[irq].gp && !(seen_irq[irq >> 5] & (1u << (irq & 31)))) {
        char text[64];
        seen_irq[irq >> 5] |= 1u << (irq & 31);
        ob_format(text, sizeof(text), "[openbios:iop] first module interrupt %#x\n", irq);
        ob_puts(text);
    }
    __asm__ volatile ("move %0, $28" : "=r"(saved));
    if (handlers[irq].gp) __asm__ volatile ("move $28, %0" :: "r"(handlers[irq].gp));
    r = h(handlers[irq].arg);
    __asm__ volatile ("move $28, %0" :: "r"(saved));
    return r;
}
static void dispatch_dma(void) {
    uint32_t d1 = DICR, d2 = DICR2, flags, ch;
    flags = (d1 >> 24) & (d1 >> 16) & 0x7f;
    for (ch = 0; ch < 7; ++ch) if (flags & (1u << ch)) {
        DICR = (DICR & 0x00ffffffu) | (1u << (24 + ch)); /* Flags are write-one-to-clear. */
        if (!call_handler(0x20 + (int)ch)) DICR = (DICR & 0x00ffffffu) & ~(1u << (16 + ch));
    }
    flags = (d2 >> 24) & (d2 >> 16) & 0x3f;
    for (ch = 0; ch < 6; ++ch) if (flags & (1u << ch)) {
        DICR2 = (DICR2 & 0x00ffffffu) | (1u << (24 + ch));
        if (!call_handler(0x28 + (int)ch)) DICR2 = (DICR2 & 0x00ffffffu) & ~(1u << (16 + ch));
    }
    DICR = (DICR & 0x00ffffffu) | 0x80000000u; /* Clear the master flag. */
}
static void dispatch_interrupts(void) {
    uint32_t pending = I_STAT & I_MASK;
    int irq;
    for (irq = 0; irq < 32; ++irq) {
        if (!(pending & (1u << irq))) continue;
        I_STAT = ~(1u << irq); /* Writing 0 acknowledges. */
        if (irq == INTR_TIMER5) {
            ++ticks;
            obk_tick(&k, (uint64_t)ticks * TICK_CYCLES);
            run_alarms((uint64_t)ticks * TICK_CYCLES);
#ifdef OB_TRACE_RPC
            if (ticks % 2000 == 0) {
                char text[24];
                unsigned j;
                ob_puts("[openbios:iopirqs]");
                for (j = 0; j < 0x30; ++j) if (irq_counts[j]) {
                    ob_format(text, sizeof(text), " %x:%u", j, irq_counts[j]);
                    ob_puts(text);
                }
                ob_puts("\n");
            }
#endif
            ob_iop_disc_tick();
            continue;
        }
        if (irq == INTR_DMA) { dispatch_dma(); continue; }
        if (!call_handler(irq)) I_MASK &= ~(1u << irq);
    }
}

struct obk_context *ob_iop_exception(struct obk_context *ctx) {
    uint32_t code = (ctx->cause >> 2) & 31;
    int next;
    char text[96];
    in_interrupt = 1;
    if (code == 8) ctx->epc += 4; /* syscall: reschedule request */
    else if (code == 0) dispatch_interrupts();
    else {
        uint32_t badvaddr;
        __asm__ volatile ("mfc0 %0, $8\n\tnop" : "=r"(badvaddr));
        ob_format(text, sizeof(text), "[openbios:iop] fatal exception code=%u epc=%08x badvaddr=%08x thread=%d\n",
                  code, ctx->epc, badvaddr, k.current);
        ob_puts(text);
        ob_halt();
    }
    in_interrupt = 0;
    next = obk_pick(&k);
    if (!next) {
        ob_puts("[openbios:iop] fatal: no runnable thread\n");
        ob_halt();
    }
    if (next != k.current) obk_switch(&k, next);
    ob_iop_context = &obk_thread(&k, k.current)->context;
    return ob_iop_context;
}

static void vblank_setup(void);
static void idle(void *arg) { (void)arg; for (;;) __asm__ volatile ("nop"); }
static int idle_thread;

/* IOP reboot: drop every module-owned kernel object and interrupt handler.
   The service loop (thread 1) and the idle thread survive. */
void ob_iop_kernel_reset(void) {
    uint32_t s = lock();
    unsigned i;
    for (i = 0; i < OBK_THREADS; ++i) {
        int id = (int)i + 1;
        if (id != 1 && id != idle_thread && k.threads[i].used) {
            if (k.threads[i].stack) ob_sysmem_free((void *)(uintptr_t)k.threads[i].stack);
            ob_zero(&k.threads[i], sizeof(k.threads[i]));
        }
    }
    ob_zero(k.semas, sizeof(k.semas));
    ob_zero(k.events, sizeof(k.events));
    for (i = 0; i < OBK_FPLS; ++i) if (k.fpls[i].used) ob_sysmem_free(k.fpls[i].mem);
    for (i = 0; i < OBK_VPLS; ++i) if (k.vpls[i].used) ob_sysmem_free(k.vpls[i].mem);
    ob_zero(k.fpls, sizeof(k.fpls));
    ob_zero(k.vpls, sizeof(k.vpls));
    ob_zero(k.mbxs, sizeof(k.mbxs));
    ob_zero(handlers, sizeof(handlers));
    I_MASK = (1u << INTR_TIMER5) | (1u << INTR_DMA);
    DICR = (DICR & 0x00800000u);
    DICR2 = 0;
    vblank_setup();
    unlock(s);
}

void ob_iop_kernel_init(void) {
    int id;
    obk_init(&k);
    obk_adopt_current(&k, OB_IOP_SERVICE_PRIORITY);
    ob_iop_context = &obk_thread(&k, 1)->context;
    id = obk_thread_create(&k, 0, 0, (uint32_t)(uintptr_t)idle, 0x00080400u, 0x400, OBK_PRIORITY_MAX, 0);
    idle_thread = id;
    obk_thread(&k, id)->init_priority = OBK_PRIORITY_IDLE;
    obk_thread_start(&k, id, 0, (uint32_t)(uintptr_t)ob_iop_thread_return);
    ob_iop_vector_install();
    I_CTRL = 0;
    I_MASK = 0;
    I_STAT = 0;
    T5_MODE = 0x58; /* reset on target, IRQ on target, repeat */
    T5_TARGET = TICK_CYCLES;
    T5_COUNT = 0;
    I_MASK = (1u << INTR_TIMER5) | (1u << INTR_DMA);
    DICR = (DICR & 0x00ffffffu) | 0x00800000u; /* DMA master interrupt enable */
    vblank_setup();
    I_CTRL = 1;
    sr_write((sr_read() & ~0xff00u) | 0x0401u); /* IM2 and IEc */
}

/* ---- thbase ---- */
typedef struct { uint32_t attr, option, thread, stacksize, priority; } iop_thread_t;
typedef struct {
    uint32_t attr, option; int status; uint32_t entry, stack; int stack_size; uint32_t gp;
    int init_priority, current_priority, wait_type, wait_id, wakeups;
    uint32_t context, reserved[4];
} iop_thread_info_t;

static int CreateThread(iop_thread_t *p) {
    uint32_t s, stack, gp = current_gp();
    int id;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    if (!p) return OBK_ILLEGAL_ENTRY;
    stack = (uint32_t)(uintptr_t)ob_sysmem_alloc(OB_ALLOC_LAST, p->stacksize, 0);
    if (!stack) return OBK_NO_MEMORY;
    s = lock();
    id = obk_thread_create(&k, p->attr, p->option, p->thread, stack, (p->stacksize + 255) & ~255u,
                           (int)p->priority, gp);
    unlock(s);
    if (id < 0) ob_sysmem_free((void *)(uintptr_t)stack);
    /* Fill the new stack with 0xff, except its top 0x30 bytes, unless
       TH_NO_FILLSTACK is set. */
    else if (!(p->attr & 0x00100000u)) {
        uint32_t *w = (uint32_t *)(uintptr_t)stack, n = (((p->stacksize + 255) & ~255u) - 0x30) / 4;
        while (n--) *w++ = 0xffffffffu;
    }
    return id;
}
/* StartThread zeroes the 0xb8-byte register area at the stack top, so the
   entry function's first frame reads zeros; some modules depend on this. */
static void clear_start_frame(int id) {
    struct obk_thread *t = obk_thread(&k, id);
    if (t && t->stack && t->stack_size >= 0xb8) ob_zero((void *)(uintptr_t)(t->stack + t->stack_size - 0xb8), 0xb8);
}
static int DeleteThread(int id) {
    uint32_t s = lock(), stack = 0;
    struct obk_thread *t = obk_thread(&k, id);
    int r;
    if (t) stack = t->stack;
    r = obk_thread_delete(&k, id);
    unlock(s);
    if (!r && stack) ob_sysmem_free((void *)(uintptr_t)stack);
    return r;
}
static int StartThread(int id, uint32_t arg) {
    uint32_t s = lock();
    int r = obk_thread_start(&k, id, arg, (uint32_t)(uintptr_t)ob_iop_thread_return);
    if (!r) clear_start_frame(id);
    if (!r) maybe_preempt();
    unlock(s);
    return r;
}
/* Internal: create and start a kernel-owned thread (built-in services). */
int ob_kernel_spawn(void (*entry)(void *), void *arg, int priority, uint32_t stack_size) {
    iop_thread_t p;
    int id;
    p.attr = 0; p.option = 0; p.thread = (uint32_t)(uintptr_t)entry;
    p.stacksize = stack_size; p.priority = (uint32_t)priority;
    id = CreateThread(&p);
    if (id > 0) StartThread(id, (uint32_t)(uintptr_t)arg);
    return id;
}
void ob_iop_exit_thread(void) {
    lock();
    obk_thread_exit(&k);
    block_now();
    for (;;) {}
}
static int ExitThread(void) { ob_iop_exit_thread(); return 0; }
static int ExitDeleteThread(void) { ob_iop_exit_thread(); return 0; } /* Stack kept. */
static int TerminateThread(int id) { uint32_t s = lock(); int r = obk_thread_terminate(&k, id); unlock(s); return r; }
static int ChangeThreadPriority(int id, int priority) {
    uint32_t s = lock();
    int r = obk_thread_priority(&k, id, priority);
    if (!r) maybe_preempt();
    unlock(s);
    return r;
}
static int iChangeThreadPriority(int id, int priority) { return obk_thread_priority(&k, id, priority); }
static int iRotateThreadReadyQueue(int priority) { return obk_thread_rotate(&k, priority); }
static int iTerminateThread(int id) { return obk_thread_terminate(&k, id); }
static int RotateThreadReadyQueue(int priority) {
    uint32_t s = lock(); int r = obk_thread_rotate(&k, priority);
    if (!r) maybe_preempt();
    unlock(s); return r;
}
static int ReleaseWaitThread(int id) {
    uint32_t s = lock(); int r = obk_thread_release_wait(&k, id);
    if (!r) maybe_preempt();
    unlock(s); return r;
}
static int iReleaseWaitThread(int id) { return obk_thread_release_wait(&k, id); }
static int GetThreadId(void) { return k.current; }
/* Bytes left between the caller's stack pointer and its stack base. */
static int CheckThreadStack(void) {
    struct obk_thread *t = obk_thread(&k, k.current);
    uint32_t sp;
    __asm__ volatile ("move %0, $29" : "=r"(sp));
    if (!t || !t->stack) return 0;
    return (int)((sp & 0x1fffffu) - (t->stack & 0x1fffffu));
}
static int refer_thread(int id, iop_thread_info_t *info) {
    struct obk_thread *t = obk_thread(&k, id ? id : k.current);
    if (!t) return OBK_UNKNOWN_THID;
    ob_zero(info, sizeof(*info));
    info->attr = t->attr; info->option = t->option; info->status = t->status;
    info->entry = t->entry; info->stack = t->stack; info->stack_size = (int)t->stack_size;
    info->gp = t->gp; info->init_priority = t->init_priority;
    info->current_priority = t->priority; info->wait_type = t->wait_type;
    info->wait_id = t->wait_id; info->wakeups = t->wakeups;
    return OBK_OK;
}
static int ReferThreadStatus(int id, iop_thread_info_t *info) {
    uint32_t s = lock(); int r = refer_thread(id, info); unlock(s); return r;
}
static int iReferThreadStatus(int id, iop_thread_info_t *info) { return refer_thread(id, info); }
static int SleepThread(void) {
    uint32_t s;
    int r;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock();
    r = obk_sleep(&k);
    if (r == OBK_BLOCK) r = block_now();
    unlock(s);
    return r;
}
static int WakeupThread(int id) {
    uint32_t s = lock(); int r = obk_wakeup(&k, id);
    if (!r) maybe_preempt();
    unlock(s); return r;
}
static int iWakeupThread(int id) { return obk_wakeup(&k, id); }
static int CancelWakeupThread(int id) { uint32_t s = lock(); int r = obk_cancel_wakeup(&k, id); unlock(s); return r; }
static int iCancelWakeupThread(int id) { return obk_cancel_wakeup(&k, id); }
static int DelayThread(uint32_t usec) {
    uint32_t s;
    int r;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock();
    obk_tick(&k, ob_iop_system_time());
    r = obk_delay(&k, ((uint64_t)usec * 4608u + 124) / 125);
    if (r == OBK_BLOCK) r = block_now();
    unlock(s);
    return r;
}
typedef struct { uint32_t lo, hi; } iop_sys_clock_t;
static void GetSystemTime(iop_sys_clock_t *c) {
    uint64_t t = ob_iop_system_time();
    c->lo = (uint32_t)t; c->hi = (uint32_t)(t >> 32);
}
static uint32_t GetSystemTimeLow(void) { return (uint32_t)ob_iop_system_time(); }
static void USec2SysClock(uint32_t usec, iop_sys_clock_t *c) {
    uint64_t t = (uint64_t)usec * 4608u / 125u;
    c->lo = (uint32_t)t; c->hi = (uint32_t)(t >> 32);
}
static void SysClock2USec(iop_sys_clock_t *c, uint32_t *sec, uint32_t *usec) {
    uint64_t us = (((uint64_t)c->hi << 32) | c->lo) * 125u / 4608u;
    *sec = (uint32_t)(us / 1000000u);
    *usec = (uint32_t)(us % 1000000u);
}
/* Alarms (thbase): the callback runs in interrupt context when the delay has
   elapsed; a nonzero return re-arms it that many clocks after the previous
   expiry. Expiry is checked on the kernel tick (1 ms). */
#define ALARMS 64
static struct { uint32_t cb, arg, gp; uint64_t target; } alarms[ALARMS];
static int add_alarm(iop_sys_clock_t *c, uint32_t cb, uint32_t arg, uint32_t gp) {
    uint64_t delay;
    unsigned i, free_slot = ALARMS;
    if (!c || !cb) return OBK_ILLEGAL_ENTRY;
    delay = ((uint64_t)c->hi << 32) | c->lo;
    for (i = 0; i < ALARMS; ++i) {
        if (alarms[i].cb == cb && alarms[i].arg == arg) return KE_FOUND_HANDLER;
        if (!alarms[i].cb && free_slot == ALARMS) free_slot = i;
    }
    if (free_slot == ALARMS) return OBK_NO_MEMORY;
    alarms[free_slot].arg = arg;
    alarms[free_slot].gp = gp;
    alarms[free_slot].target = ob_iop_system_time() + (delay ? delay : 1);
    alarms[free_slot].cb = cb;
    return OBK_OK;
}
static int remove_alarm(uint32_t cb, uint32_t arg) {
    unsigned i;
    for (i = 0; i < ALARMS; ++i)
        if (alarms[i].cb == cb && alarms[i].arg == arg) { alarms[i].cb = 0; return OBK_OK; }
    return KE_NOTFOUND_ALARM;
}
static void run_alarms(uint64_t now) {
    unsigned i;
    for (i = 0; i < ALARMS; ++i) {
        uint32_t (*cb)(uint32_t), saved, again;
        if (!alarms[i].cb || alarms[i].target > now) continue;
        cb = (uint32_t (*)(uint32_t))(uintptr_t)alarms[i].cb;
        __asm__ volatile ("move %0, $28" : "=r"(saved));
        if (alarms[i].gp) __asm__ volatile ("move $28, %0" :: "r"(alarms[i].gp));
        again = cb(alarms[i].arg);
        __asm__ volatile ("move $28, %0" :: "r"(saved));
        if (!again) alarms[i].cb = 0;
        else {
            alarms[i].target += again;
            if (alarms[i].target <= now) alarms[i].target = now + again;
        }
    }
}
static int SetAlarm(iop_sys_clock_t *c, uint32_t cb, uint32_t arg) {
    uint32_t s; int r;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock(); r = add_alarm(c, cb, arg, current_gp()); unlock(s);
    return r;
}
static int iSetAlarm(iop_sys_clock_t *c, uint32_t cb, uint32_t arg) {
    return add_alarm(c, cb, arg, current_gp());
}
static int CancelAlarm(uint32_t cb, uint32_t arg) {
    uint32_t s; int r;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock(); r = remove_alarm(cb, arg); unlock(s);
    return r;
}
static int iCancelAlarm(uint32_t cb, uint32_t arg) { return remove_alarm(cb, arg); }
static int GetThreadCurrentPriority(void) { return obk_thread(&k, k.current)->priority; }

/* ---- thsemap ---- */
typedef struct { uint32_t attr, option; int initial, max; } iop_sema_t;
typedef struct { uint32_t attr, option; int initial, max, current, waiting, reserved[2]; } iop_sema_info_t;
static int CreateSema(iop_sema_t *p) {
    uint32_t s; int r;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock(); r = obk_sema_create(&k, p->attr, p->option, p->initial, p->max); unlock(s);
    return r;
}
static int DeleteSema(int id) {
    uint32_t s = lock(); int r = obk_sema_delete(&k, id);
    if (!r) maybe_preempt();
    unlock(s); return r;
}
static int SignalSema(int id) {
    uint32_t s = lock(); int r = obk_sema_signal(&k, id);
    if (!r) maybe_preempt();
    unlock(s); return r;
}
static int iSignalSema(int id) { return obk_sema_signal(&k, id); }
static int WaitSema(int id) {
    uint32_t s; int r;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock(); r = obk_sema_wait(&k, id, 0);
    if (r == OBK_BLOCK) r = block_now();
    unlock(s); return r;
}
static int PollSema(int id) { uint32_t s = lock(); int r = obk_sema_wait(&k, id, 1); unlock(s); return r; }
static int refer_sema(int id, iop_sema_info_t *i) {
    ob_zero(i, sizeof(*i));
    return obk_sema_status(&k, id, &i->current, &i->max, &i->initial, &i->waiting, &i->attr, &i->option);
}
static int ReferSemaStatus(int id, iop_sema_info_t *i) { uint32_t s = lock(); int r = refer_sema(id, i); unlock(s); return r; }
static int iReferSemaStatus(int id, iop_sema_info_t *i) { return refer_sema(id, i); }

/* ---- thevent ---- */
typedef struct { uint32_t attr, option, bits; } iop_event_t;
typedef struct { uint32_t attr, option, initial, current; int waiting, reserved[2]; } iop_event_info_t;
static int CreateEventFlag(iop_event_t *p) {
    uint32_t s; int r;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock(); r = obk_event_create(&k, p->attr, p->option, p->bits); unlock(s);
    return r;
}
static int DeleteEventFlag(int id) {
    uint32_t s = lock(); int r = obk_event_delete(&k, id);
    if (!r) maybe_preempt();
    unlock(s); return r;
}
static int SetEventFlag(int id, uint32_t bits) {
    uint32_t s = lock(); int r = obk_event_set(&k, id, bits);
    if (!r) maybe_preempt();
    unlock(s); return r;
}
static int iSetEventFlag(int id, uint32_t bits) { return obk_event_set(&k, id, bits); }
static int ClearEventFlag(int id, uint32_t bits) { uint32_t s = lock(); int r = obk_event_clear(&k, id, bits); unlock(s); return r; }
static int iClearEventFlag(int id, uint32_t bits) { return obk_event_clear(&k, id, bits); }
static int wait_event(int id, uint32_t bits, int mode, uint32_t *result, int poll) {
    uint32_t s, value = 0; int r;
    if (in_interrupt && !poll) return OBK_ILLEGAL_CONTEXT;
    s = lock();
    r = obk_event_wait(&k, id, bits, (uint32_t)mode, poll, &value);
    if (r == OBK_BLOCK) {
        r = block_now();
        value = obk_thread(&k, k.current)->event_result;
    }
    unlock(s);
    if (r == OBK_OK && result) *result = value;
    return r;
}
static int WaitEventFlag(int id, uint32_t bits, int mode, uint32_t *result) { return wait_event(id, bits, mode, result, 0); }
static int PollEventFlag(int id, uint32_t bits, int mode, uint32_t *result) { return wait_event(id, bits, mode, result, 1); }
static int refer_event(int id, iop_event_info_t *i) {
    ob_zero(i, sizeof(*i));
    return obk_event_status(&k, id, &i->current, &i->initial, &i->waiting, &i->attr, &i->option);
}
static int ReferEventFlagStatus(int id, iop_event_info_t *i) { uint32_t s = lock(); int r = refer_event(id, i); unlock(s); return r; }
static int iReferEventFlagStatus(int id, iop_event_info_t *i) { return refer_event(id, i); }

/* ---- thfpool / thvpool / thmsgbx ---- */
#define ERRPTR(r) ((void *)(intptr_t)(r))
static void *pool_memory(uint32_t attr, int bytes) {
    return ob_sysmem_alloc((attr & OBK_PA_MEMBTM) ? OB_ALLOC_LAST : OB_ALLOC_FIRST, (uint32_t)bytes, 0);
}
/* Shared tail of the allocate calls: block if needed and pick up the result. */
static void *pool_result(int r, void *p) {
    if (r == OBK_BLOCK) {
        r = block_now();
        p = obk_thread(&k, k.current)->wait_ptr;
    }
    return r ? ERRPTR(r) : p;
}
typedef struct { uint32_t attr, option; int block_size, blocks; } iop_fpl_param;
typedef struct { uint32_t attr, option; int block_size, blocks, free_blocks, waiting, reserved[4]; } iop_fpl_info_t;
static int CreateFpl(iop_fpl_param *p) {
    uint32_t s; int r; void *mem;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    r = obk_fpl_bytes(p->attr, p->block_size, p->blocks);
    if (r < 0) return r;
    if (!(mem = pool_memory(p->attr, r))) return OBK_NO_MEMORY;
    s = lock(); r = obk_fpl_create(&k, p->attr, p->option, p->block_size, p->blocks, mem); unlock(s);
    if (r < 0) ob_sysmem_free(mem);
    return r;
}
static int DeleteFpl(int id) {
    uint32_t s; int r; void *mem = 0;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock(); r = obk_fpl_delete(&k, id, &mem);
    if (!r) { ob_sysmem_free(mem); maybe_preempt(); }
    unlock(s); return r;
}
static void *AllocateFpl(int id) {
    uint32_t s; void *p = 0, *q; int r;
    if (in_interrupt) return ERRPTR(OBK_ILLEGAL_CONTEXT);
    s = lock(); r = obk_fpl_alloc(&k, id, 0, &p); q = pool_result(r, p); unlock(s);
    return q;
}
static void *pAllocateFpl(int id) {
    uint32_t s; void *p = 0; int r;
    if (in_interrupt) return ERRPTR(OBK_ILLEGAL_CONTEXT);
    s = lock(); r = obk_fpl_alloc(&k, id, 1, &p); unlock(s);
    return r ? ERRPTR(r) : p;
}
static void *ipAllocateFpl(int id) {
    uint32_t s = lock(); void *p = 0; int r = obk_fpl_alloc(&k, id, 1, &p); unlock(s);
    return r ? ERRPTR(r) : p;
}
static int FreeFpl(int id, void *block) {
    uint32_t s; int r;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock(); r = obk_fpl_free(&k, id, block);
    if (!r) maybe_preempt();
    unlock(s); return r;
}
static int refer_fpl(int id, iop_fpl_info_t *i) {
    uint32_t s = lock(); int r;
    ob_zero(i, sizeof(*i));
    r = obk_fpl_status(&k, id, &i->attr, &i->option, &i->block_size, &i->blocks, &i->free_blocks, &i->waiting);
    unlock(s); return r;
}
static int ReferFplStatus(int id, iop_fpl_info_t *i) { return in_interrupt ? OBK_ILLEGAL_CONTEXT : refer_fpl(id, i); }
static int iReferFplStatus(int id, iop_fpl_info_t *i) { return refer_fpl(id, i); }

typedef struct { uint32_t attr, option; int size; } iop_vpl_param;
typedef struct { uint32_t attr, option; int size, free_size, waiting, reserved[3]; } iop_vpl_info_t;
static int CreateVpl(iop_vpl_param *p) {
    uint32_t s; int r; void *mem;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    r = obk_vpl_bytes(p->attr, p->size);
    if (r < 0) return r;
    if (!(mem = pool_memory(p->attr, r))) return OBK_NO_MEMORY;
    s = lock(); r = obk_vpl_create(&k, p->attr, p->option, p->size, mem); unlock(s);
    if (r < 0) ob_sysmem_free(mem);
    return r;
}
static int DeleteVpl(int id) {
    uint32_t s; int r; void *mem = 0;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock(); r = obk_vpl_delete(&k, id, &mem);
    if (!r) { ob_sysmem_free(mem); maybe_preempt(); }
    unlock(s); return r;
}
static void *AllocateVpl(int id, int size) {
    uint32_t s; void *p = 0, *q; int r;
    if (in_interrupt) return ERRPTR(OBK_ILLEGAL_CONTEXT);
    s = lock(); r = obk_vpl_alloc(&k, id, size, 0, &p); q = pool_result(r, p); unlock(s);
    return q;
}
static void *pAllocateVpl(int id, int size) {
    uint32_t s; void *p = 0; int r;
    if (in_interrupt) return ERRPTR(OBK_ILLEGAL_CONTEXT);
    s = lock(); r = obk_vpl_alloc(&k, id, size, 1, &p); unlock(s);
    return r ? ERRPTR(r) : p;
}
static void *ipAllocateVpl(int id, int size) {
    uint32_t s = lock(); void *p = 0; int r = obk_vpl_alloc(&k, id, size, 1, &p); unlock(s);
    return r ? ERRPTR(r) : p;
}
static int FreeVpl(int id, void *block) {
    uint32_t s; int r;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock(); r = obk_vpl_free(&k, id, block);
    if (!r) maybe_preempt();
    unlock(s); return r;
}
static int refer_vpl(int id, iop_vpl_info_t *i) {
    uint32_t s = lock(); int r;
    ob_zero(i, sizeof(*i));
    r = obk_vpl_status(&k, id, &i->attr, &i->option, &i->size, &i->free_size, &i->waiting);
    unlock(s); return r;
}
static int ReferVplStatus(int id, iop_vpl_info_t *i) { return in_interrupt ? OBK_ILLEGAL_CONTEXT : refer_vpl(id, i); }
static int iReferVplStatus(int id, iop_vpl_info_t *i) { return refer_vpl(id, i); }

typedef struct { uint32_t attr, option; } iop_mbx_t;
typedef struct { uint32_t attr, option; int waiting, count; void *top; int reserved[2]; } iop_mbx_status_t;
static int CreateMbx(iop_mbx_t *p) {
    uint32_t s; int r;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock(); r = obk_mbx_create(&k, p->attr, p->option); unlock(s);
    return r;
}
static int DeleteMbx(int id) {
    uint32_t s; int r;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock(); r = obk_mbx_delete(&k, id);
    if (!r) maybe_preempt();
    unlock(s); return r;
}
static int SendMbx(int id, void *msg) {
    uint32_t s; int r;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock(); r = obk_mbx_send(&k, id, msg);
    if (!r) maybe_preempt();
    unlock(s); return r;
}
static int iSendMbx(int id, void *msg) { uint32_t s = lock(); int r = obk_mbx_send(&k, id, msg); unlock(s); return r; }
static int receive_mbx(void **out, int id, int poll) {
    uint32_t s; int r; struct obk_message *m = 0;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock();
    r = obk_mbx_receive(&k, id, poll, &m);
    if (r == OBK_BLOCK) {
        r = block_now();
        m = obk_thread(&k, k.current)->wait_ptr;
    }
    unlock(s);
    if (!r && out) *out = m;
    return r;
}
static int ReceiveMbx(void **out, int id) { return receive_mbx(out, id, 0); }
static int PollMbx(void **out, int id) { return receive_mbx(out, id, 1); }
static int refer_mbx(int id, iop_mbx_status_t *i) {
    uint32_t s = lock(); int r; struct obk_message *top = 0;
    ob_zero(i, sizeof(*i));
    r = obk_mbx_status(&k, id, &i->attr, &i->option, &i->waiting, &i->count, &top);
    i->top = top;
    unlock(s); return r;
}
static int ReferMbxStatus(int id, iop_mbx_status_t *i) { return in_interrupt ? OBK_ILLEGAL_CONTEXT : refer_mbx(id, i); }
static int iReferMbxStatus(int id, iop_mbx_status_t *i) { return refer_mbx(id, i); }

/* ---- intrman ---- */
#define KE_ILLEGAL_INTRCODE (-101)
#define KE_CPUDI (-102)
#define KE_INTRDISABLE (-103)
#define KE_NOTFOUND_HANDLER (-105)
static int valid_irq(int irq) { return (irq >= 0 && irq < 32) || (irq >= 0x20 && irq <= 0x2d); }
static int RegisterIntrHandler(int irq, int mode, int (*handler)(void *), void *arg) {
    uint32_t s;
    if (irq < 0 || irq >= 64 || !valid_irq(irq)) return KE_ILLEGAL_INTRCODE;
    s = lock();
    if (handlers[irq].handler) { unlock(s); return KE_FOUND_HANDLER; }
    handlers[irq].handler = handler;
    handlers[irq].arg = arg;
    handlers[irq].mode = mode;
    handlers[irq].gp = current_gp();
    unlock(s);
    {
        char text[80];
        ob_format(text, sizeof(text), "[openbios:iop] module interrupt handler %#x mode %d at %08x\n",
                  irq, mode, (uint32_t)(uintptr_t)handler);
        ob_puts(text);
    }
    return OBK_OK;
}
static int ReleaseIntrHandler(int irq) {
    uint32_t s;
    if (irq < 0 || irq >= 64) return KE_ILLEGAL_INTRCODE;
    s = lock();
    if (!handlers[irq].handler) { unlock(s); return KE_NOTFOUND_HANDLER; }
    handlers[irq].handler = 0;
    unlock(s);
    return OBK_OK;
}
static int set_enable(int irq, int enable) {
    uint32_t s = lock(), bit, before;
    if (irq < 0x20) {
        bit = 1u << irq;
        before = I_MASK & bit;
        if (enable) I_MASK |= bit; else I_MASK &= ~bit;
    } else if (irq < 0x27) {
        bit = 1u << (16 + irq - 0x20);
        before = DICR & bit;
        DICR = enable ? ((DICR & 0x00ffffffu) | bit) : ((DICR & 0x00ffffffu) & ~bit);
    } else {
        bit = 1u << (16 + irq - 0x28);
        before = DICR2 & bit;
        DICR2 = enable ? ((DICR2 & 0x00ffffffu) | bit) : ((DICR2 & 0x00ffffffu) & ~bit);
    }
    unlock(s);
    return before != 0;
}
/* Internal thread API for other built-in libraries. */
int ob_kernel_wakeup(int id) {
    uint32_t s = lock();
    int r = obk_wakeup(&k, id);
    if (!r) maybe_preempt();
    unlock(s);
    return r;
}
int ob_kernel_sleep(void) { return SleepThread(); }
int ob_kernel_pause(uint32_t usec) {
    if (in_interrupt || !(sr_read() & 1u)) return 0;
    DelayThread(usec);
    return 1;
}
int ob_kernel_current_thread(void) { return k.current; }
int ob_kernel_set_priority(int priority) {
    int old = obk_thread(&k, k.current)->priority;
    ChangeThreadPriority(0, priority);
    return old;
}
/* Internal interrupt API for other built-in libraries. */
int ob_intr_register(int irq, int (*handler)(void *), void *arg) {
    uint32_t s = lock();
    if (handlers[irq].handler) { unlock(s); return KE_FOUND_HANDLER; }
    handlers[irq].handler = handler;
    handlers[irq].arg = arg;
    handlers[irq].gp = 0;
    unlock(s);
    return OBK_OK;
}
void ob_intr_release(int irq) { handlers[irq].handler = 0; }
void ob_intr_enable(int irq, int enable) { set_enable(irq, enable); }
int ob_intr_context(void) { return in_interrupt; }
static int EnableIntr(int irq) {
    if (!valid_irq(irq) || irq == 0x27) return KE_ILLEGAL_INTRCODE;
    set_enable(irq, 1);
    return OBK_OK;
}
static int DisableIntr(int irq, int *previous) {
    int was;
    if (!valid_irq(irq) || irq == 0x27) return KE_ILLEGAL_INTRCODE;
    was = set_enable(irq, 0);
    if (previous) *previous = was ? irq : -1;
    return was ? OBK_OK : KE_INTRDISABLE;
}
static int CpuSuspendIntr(int *state) {
    uint32_t s = lock();
    if (state) *state = (int)(s & 1u);
    return (s & 1u) ? OBK_OK : KE_CPUDI;
}
static int CpuResumeIntr(int state) {
    if (state & 1) sr_write(sr_read() | 1u);
    return OBK_OK;
}
static int CpuDisableIntr(void) { uint32_t s = lock(); return (s & 1u) ? OBK_OK : KE_CPUDI; }
static int CpuEnableIntr(void) { sr_write(sr_read() | 1u); return OBK_OK; }
static int QueryIntrContext(void) { return in_interrupt; }

/* ---- vblank ---- */
#define VB_START 1u
#define VB_IN 2u
#define VB_END 4u
#define VB_OUT 8u
#define VB_HANDLERS 16
static int vblank_flag;
static struct vb_item { int (*fn)(void *); void *arg; int priority; uint32_t gp; } vb_items[2][VB_HANDLERS];
static int vb_count[2];
static void vb_run(int edge) {
    int i = 0;
    while (i < vb_count[edge]) {
        struct vb_item *it = &vb_items[edge][i];
        uint32_t saved;
        int keep;
        __asm__ volatile ("move %0, $28" : "=r"(saved));
        if (it->gp) __asm__ volatile ("move $28, %0" :: "r"(it->gp));
        keep = it->fn(it->arg);
        __asm__ volatile ("move $28, %0" :: "r"(saved));
        if (keep) { ++i; continue; }
        ob_copy(it, it + 1, (uint32_t)(vb_count[edge] - i - 1) * sizeof(*it));
        --vb_count[edge];
    }
}
static int vblank_start_irq(void *arg) {
    (void)arg;
    obk_event_set(&k, vblank_flag, VB_START | VB_IN);
    obk_event_clear(&k, vblank_flag, ~(VB_END | VB_OUT));
    vb_run(0);
    return 1;
}
static int vblank_end_irq(void *arg) {
    (void)arg;
    obk_event_set(&k, vblank_flag, VB_END | VB_OUT);
    obk_event_clear(&k, vblank_flag, ~(VB_START | VB_IN));
    vb_run(1);
    return 1;
}
static void vblank_setup(void) {
    vblank_flag = obk_event_create(&k, OBK_EA_MULTI, 0, 0);
    ob_zero(vb_items, sizeof(vb_items));
    vb_count[0] = vb_count[1] = 0;
    handlers[0].handler = vblank_start_irq;
    handlers[0].gp = 0;
    handlers[11].handler = vblank_end_irq;
    handlers[11].gp = 0;
    I_MASK |= (1u << 0) | (1u << 11);
}
/* WaitVblankStart/End wait for the next edge; WaitVblank/WaitNonVblank only
   wait while outside/inside the blanking interval. */
static void wait_edge(uint32_t bit) {
    uint32_t s = lock();
    obk_event_clear(&k, vblank_flag, ~bit);
    unlock(s);
    wait_event(vblank_flag, bit, OBK_WEF_OR, 0, 0);
}
static void WaitVblankStart(void) { wait_edge(VB_START); }
static void WaitVblankEnd(void) { wait_edge(VB_END); }
static void WaitVblank(void) { wait_event(vblank_flag, VB_IN, OBK_WEF_OR, 0, 0); }
static void WaitNonVblank(void) { wait_event(vblank_flag, VB_OUT, OBK_WEF_OR, 0, 0); }
static int RegisterVblankHandler(int edge, int priority, int (*fn)(void *), void *arg) {
    uint32_t s;
    int e = edge ? 1 : 0, i, at;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock();
    for (i = 0; i < vb_count[e]; ++i)
        if (vb_items[e][i].fn == fn) { unlock(s); return KE_FOUND_HANDLER; }
    if (vb_count[e] == VB_HANDLERS) { unlock(s); return OBK_NO_MEMORY; }
    for (at = 0; at < vb_count[e] && vb_items[e][at].priority <= priority; ++at) {}
    for (i = vb_count[e]; i > at; --i) vb_items[e][i] = vb_items[e][i - 1];
    vb_items[e][at].fn = fn;
    vb_items[e][at].arg = arg;
    vb_items[e][at].priority = priority;
    vb_items[e][at].gp = current_gp();
    ++vb_count[e];
    unlock(s);
    return OBK_OK;
}
static int ReleaseVblankHandler(int edge, int (*fn)(void *)) {
    uint32_t s;
    int e = edge ? 1 : 0, i;
    if (in_interrupt) return OBK_ILLEGAL_CONTEXT;
    s = lock();
    for (i = 0; i < vb_count[e]; ++i) if (vb_items[e][i].fn == fn) {
        ob_copy(&vb_items[e][i], &vb_items[e][i + 1], (uint32_t)(vb_count[e] - i - 1) * sizeof(vb_items[e][0]));
        --vb_count[e];
        unlock(s);
        return OBK_OK;
    }
    unlock(s);
    return KE_NOTFOUND_HANDLER;
}

#define LIBRARY(var, text, major, minor, size) \
    static struct { uint32_t magic, next; uint16_t version, mode; \
                    char name[8] __attribute__((nonstring)); uint32_t f[size + 1]; } var = { \
        OB_IRX_EXPORT_MAGIC, 0, (major) << 8 | (minor), 0, text, {0}}
#define FN(p) ((uint32_t)(uintptr_t)(p))
static int ret_zero(void) { return 0; }
LIBRARY(thbase, "thbase", 1, 2, 48);
LIBRARY(thsemap, "thsemap", 1, 1, 13);
LIBRARY(thevent, "thevent", 1, 1, 15);
LIBRARY(intrman, "intrman", 1, 2, 32);
LIBRARY(vblank, "vblank", 1, 1, 10);
LIBRARY(thfpool, "thfpool", 1, 1, 13);
LIBRARY(thvpool, "thvpool", 1, 1, 13);
LIBRARY(thmsgbx, "thmsgbx", 1, 1, 13);
void ob_iop_heaplib_init(void);

void ob_iop_kernel_libraries_init(void) {
    unsigned i;
    for (i = 0; i < 4; ++i)
        thbase.f[i] = thsemap.f[i] = thevent.f[i] = intrman.f[i] = FN(ret_zero);
    thbase.f[4] = FN(CreateThread);   thbase.f[5] = FN(DeleteThread);
    thbase.f[6] = FN(StartThread);    thbase.f[8] = FN(ExitThread);
    thbase.f[9] = FN(ExitDeleteThread); thbase.f[10] = FN(TerminateThread);
    thbase.f[11] = FN(iTerminateThread);
    thbase.f[14] = FN(ChangeThreadPriority); thbase.f[15] = FN(iChangeThreadPriority);
    thbase.f[16] = FN(RotateThreadReadyQueue); thbase.f[17] = FN(iRotateThreadReadyQueue);
    thbase.f[18] = FN(ReleaseWaitThread); thbase.f[19] = FN(iReleaseWaitThread);
    thbase.f[20] = FN(GetThreadId);   thbase.f[21] = FN(CheckThreadStack);
    thbase.f[22] = FN(ReferThreadStatus); thbase.f[23] = FN(iReferThreadStatus);
    thbase.f[24] = FN(SleepThread);   thbase.f[25] = FN(WakeupThread);
    thbase.f[26] = FN(iWakeupThread); thbase.f[27] = FN(CancelWakeupThread);
    thbase.f[28] = FN(iCancelWakeupThread);
    thbase.f[33] = FN(DelayThread);   thbase.f[34] = FN(GetSystemTime);
    thbase.f[35] = FN(SetAlarm);      thbase.f[36] = FN(iSetAlarm);
    thbase.f[37] = FN(CancelAlarm);   thbase.f[38] = FN(iCancelAlarm);
    thbase.f[39] = FN(USec2SysClock); thbase.f[40] = FN(SysClock2USec);
    thbase.f[42] = FN(GetThreadCurrentPriority); thbase.f[43] = FN(GetSystemTimeLow);
    thsemap.f[4] = FN(CreateSema); thsemap.f[5] = FN(DeleteSema);
    thsemap.f[6] = FN(SignalSema); thsemap.f[7] = FN(iSignalSema);
    thsemap.f[8] = FN(WaitSema);   thsemap.f[9] = FN(PollSema);
    thsemap.f[11] = FN(ReferSemaStatus); thsemap.f[12] = FN(iReferSemaStatus);
    thevent.f[4] = FN(CreateEventFlag); thevent.f[5] = FN(DeleteEventFlag);
    thevent.f[6] = FN(SetEventFlag);    thevent.f[7] = FN(iSetEventFlag);
    thevent.f[8] = FN(ClearEventFlag);  thevent.f[9] = FN(iClearEventFlag);
    thevent.f[10] = FN(WaitEventFlag);  thevent.f[11] = FN(PollEventFlag);
    thevent.f[13] = FN(ReferEventFlagStatus); thevent.f[14] = FN(iReferEventFlagStatus);
    intrman.f[4] = FN(RegisterIntrHandler); intrman.f[5] = FN(ReleaseIntrHandler);
    intrman.f[6] = FN(EnableIntr);  intrman.f[7] = FN(DisableIntr);
    intrman.f[8] = FN(CpuDisableIntr); intrman.f[9] = FN(CpuEnableIntr);
    intrman.f[17] = FN(CpuSuspendIntr); intrman.f[18] = FN(CpuResumeIntr);
    intrman.f[19] = FN(CpuSuspendIntr); intrman.f[20] = FN(CpuResumeIntr);
    intrman.f[23] = FN(QueryIntrContext);
    ob_iop_register_builtin((void *)&thbase, 48);
    ob_iop_register_builtin((void *)&thsemap, 13);
    ob_iop_register_builtin((void *)&thevent, 15);
    ob_iop_register_builtin((void *)&intrman, 32);
    vblank.f[0] = vblank.f[1] = vblank.f[2] = vblank.f[3] = FN(ret_zero);
    vblank.f[4] = FN(WaitVblankStart); vblank.f[5] = FN(WaitVblankEnd);
    vblank.f[6] = FN(WaitVblank);      vblank.f[7] = FN(WaitNonVblank);
    vblank.f[8] = FN(RegisterVblankHandler); vblank.f[9] = FN(ReleaseVblankHandler);
    ob_iop_register_builtin((void *)&vblank, 10);
    for (i = 0; i < 4; ++i) thfpool.f[i] = thvpool.f[i] = thmsgbx.f[i] = FN(ret_zero);
    thfpool.f[4] = FN(CreateFpl);     thfpool.f[5] = FN(DeleteFpl);
    thfpool.f[6] = FN(AllocateFpl);   thfpool.f[7] = FN(pAllocateFpl);
    thfpool.f[8] = FN(ipAllocateFpl); thfpool.f[9] = FN(FreeFpl);
    thfpool.f[11] = FN(ReferFplStatus); thfpool.f[12] = FN(iReferFplStatus);
    thvpool.f[4] = FN(CreateVpl);     thvpool.f[5] = FN(DeleteVpl);
    thvpool.f[6] = FN(AllocateVpl);   thvpool.f[7] = FN(pAllocateVpl);
    thvpool.f[8] = FN(ipAllocateVpl); thvpool.f[9] = FN(FreeVpl);
    thvpool.f[11] = FN(ReferVplStatus); thvpool.f[12] = FN(iReferVplStatus);
    thmsgbx.f[4] = FN(CreateMbx);  thmsgbx.f[5] = FN(DeleteMbx);
    thmsgbx.f[6] = FN(SendMbx);    thmsgbx.f[7] = FN(iSendMbx);
    thmsgbx.f[8] = FN(ReceiveMbx); thmsgbx.f[9] = FN(PollMbx);
    thmsgbx.f[11] = FN(ReferMbxStatus); thmsgbx.f[12] = FN(iReferMbxStatus);
    ob_iop_register_builtin((void *)&thfpool, 13);
    ob_iop_register_builtin((void *)&thvpool, 13);
    ob_iop_register_builtin((void *)&thmsgbx, 13);
    ob_iop_heaplib_init();
}
