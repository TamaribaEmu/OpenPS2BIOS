/* SPDX-License-Identifier: MIT */
#include "openbios/ee.h"
#include "openbios/platform.h"
#include "openbios/sched.h"
#include "openbios/disc.h"

union ob_ee_trap_slot ob_ee_trap_frames[OB_EE_TRAP_LEVELS];
uint32_t ob_ee_exception_depth;
extern void ob_ee_exception_entry(void);
static uint32_t osd_config = 0x00020000u;
static uint8_t osd_extended[128];
static struct ob_scheduler scheduler;
static uint32_t heap_end;
#ifdef OB_TRACE_SYSCALLS
uint32_t setdma_calls, setdma_zero;
#endif
static uint32_t gs_imr; /* Programs start with GS IMR 0. */
static int strict_syscalls;
static uint32_t syscall_handlers[0x130];
static struct ob_ee_frame *interrupt_frame;
extern void ob_ee_idle(void);
extern void ob_ee_resume_intr_entry(void);
/* Handlers run as handler(cause, arg, interrupted PC); arg is the optional
   fourth argument of AddIntcHandler/AddDmacHandler (the SDK's "...Handler2"). */
struct intc_handler { uint32_t address, gp; int cause, next; uint32_t arg; };
/* Standard EE TLB layout: scratchpad, kernel/IO windows, then the user RAM
   windows. Libraries rewrite these indices in place, so the order matters. */
static const uint32_t kernel_tlb[13][4] = {
    {0x00000000, 0x70000000, 0x80000007, 0x00000007}, /* scratchpad */
    {0x00006000, 0xffff8000, 0x00001e1f, 0x00001f1f},
    {0, 0x10000000, 0x00400017, 0x00400053}, {0, 0x10002000, 0x00400097, 0x004000d7},
    {0, 0x10004000, 0x00400117, 0x00400157}, {0, 0x10006000, 0x00400197, 0x004001d7},
    {0, 0x10008000, 0x00400217, 0x00400257}, {0, 0x1000a000, 0x00400297, 0x004002d7},
    {0, 0x1000c000, 0x00400313, 0x00400357}, {0, 0x1000e000, 0x00400397, 0x004003d7},
    {0x0001e000, 0x11000000, 0x00440017, 0x00440415}, /* VU memory */
    {0x0001e000, 0x12000000, 0x00480017, 0x00480415}, /* GS privileged */
    {0x01ffe000, 0x1e000000, 0x00780017, 0x007c0017},
};

/* Map [start, 32 MiB) of RAM at segment `base`: 256 KiB page pairs below
   2 MiB, 1 MiB pairs below 8 MiB, then 4 MiB pairs. */
static uint32_t map_ram_window(uint32_t index, uint32_t base, uint32_t start, uint32_t flags) {
    uint32_t pa;
    for (pa = start; pa < 0x02000000u;) {
        uint32_t page = pa < 0x00200000u ? 0x40000u : pa < 0x00800000u ? 0x100000u : 0x400000u;
        ob_ee_tlb_write(index++, (page / 4096u - 1u) << 13, base + pa,
            (pa >> 6) | flags, ((pa + page) >> 6) | flags);
        pa += page * 2u;
    }
    return index;
}

int ob_ee_tlb_init_standard(void) {
    uint32_t i, used;
    ob_ee_set_wired(0);
    for (i = 0; i < 13; ++i)
        ob_ee_tlb_write(i, kernel_tlb[i][0], kernel_tlb[i][1], kernel_tlb[i][2], kernel_tlb[i][3]);
    i = map_ram_window(i, 0x00000000u, 0x00080000u, 0x1f); /* cached */
    i = map_ram_window(i, 0x20000000u, 0x00080000u, 0x17); /* uncached */
    ob_ee_set_wired(i);
    used = i = map_ram_window(i, 0x30000000u, 0x00100000u, 0x3f); /* uncached accelerated */
    for (; i < 48; ++i) ob_ee_tlb_write(i, 0, 0xe0000000u + (i << 13), 0, 0);
    return (int)used;
}

void ob_ee_tlb_init(void) { (void)ob_ee_tlb_init_standard(); }

static struct intc_handler intc_handlers[32];
/* SetVSyncFlag: at the next VBLANK start, *flag = 1 and the 64-bit GS CSR
   are stored once, then the flag is disarmed. */
static uint32_t vsync_flag, vsync_csr;
/* SBUS (IOP-to-EE) interrupt handlers, one per SMFLG bit. */
static uint32_t sbus_handlers[32];
#define SBUS_MSFLG (*(volatile uint32_t *)0xb000f220u)
#define SBUS_SMFLG (*(volatile uint32_t *)0xb000f230u)
#define SBUS_CTRL (*(volatile uint32_t *)0xb000f240u)
static int intc_heads[15];
static struct intc_handler dmac_handlers[32];
static int dmac_heads[10];
#define DMAC_STAT (*(volatile uint32_t *)0xb000e010u)
#define INTC_STAT (*(volatile uint32_t *)0xb000f000u)
#define INTC_MASK (*(volatile uint32_t *)0xb000f010u)
extern void ob_ee_thread_return(void);

_Static_assert(offsetof(struct ob_ee_frame, hi) == 512, "assembly frame HI offset");
_Static_assert(offsetof(struct ob_ee_frame, epc) == 528, "assembly frame EPC offset");
_Static_assert(offsetof(struct ob_ee_frame, hi1) == 544, "assembly frame HI1 offset");
_Static_assert(offsetof(struct ob_ee_frame, fpu) == 568, "assembly frame FPU offset");
_Static_assert(sizeof(union ob_ee_trap_slot) == 1024, "assembly trap slot stride");

static void reset_threads(void) {
    struct ob_thread *idle;
    ob_sched_init(&scheduler, (uint32_t)(uintptr_t)ob_ee_thread_return);
    idle = &scheduler.threads[OB_MAX_THREADS - 1];
    idle->state = OB_READY;
    idle->desc.current_priority = 128; /* Below every public thread priority. */
    idle->frame.epc = (uint32_t)(uintptr_t)ob_ee_idle;
    idle->frame.status = 0x70010c03u;
    idle->frame.gpr[29][0] = 0x00058000; /* below the exception stacks */
}

/* Program arguments handed to the next SetupThread: argc, then NUL-separated
   strings, matching the 16-pointer/256-byte argument area. */
static int exec_argc;
static char exec_args[256];
void ob_ee_set_exec_args(int argc, const char *const *argv) {
    int i;
    uint32_t used = 0;
    exec_argc = 0;
    for (i = 0; i < argc && i < 16; ++i) {
        const char *a = argv[i];
        uint32_t n = 0;
        if (!a) break;
        while (a[n] && used + n + 1 < sizeof(exec_args)) ++n;
        if (a[n]) break;
        ob_copy(exec_args + used, a, n + 1);
        used += n + 1;
        ++exec_argc;
    }
}

/* Exec arguments may sit anywhere in RAM, including the kernel area where
   libraries stage them before the call. */
static const void *exec_arg_pointer(uint32_t address, uint32_t size) {
    uint32_t physical = address;
    if (address >= 0x80000000u && address < 0xc0000000u) physical &= 0x1fffffffu;
    else if (address < 0x00080000u) return 0;
    if (!ob_range(physical, size, 0x02000000u)) return 0;
    return (const void *)(uintptr_t)(physical | 0xa0000000u);
}

/* Copy argv from the caller before the kernel state it lives under is reset. */
static void capture_user_args(const char *first, int argc, uint32_t argv) {
    const char *list[16];
    char copy[256];
    uint32_t used = 0;
    int n = 0, i;
    if (first) list[n++] = first;
    for (i = 0; i < argc && n < 16; ++i) {
        const uint32_t *slot = exec_arg_pointer(argv + 4u * (uint32_t)i, 4);
        const char *text = slot ? exec_arg_pointer(*slot, 1) : 0;
        uint32_t len = 0;
        if (!text) break;
        while (text[len] && used + len + 1 < sizeof(copy)) ++len;
        if (text[len]) break;
        ob_copy(copy + used, text, len + 1);
        list[n++] = copy + used;
        used += len + 1;
    }
    if (first) { /* `first` may live in exec_args itself: build in a local copy. */
        char head[256];
        uint32_t len = 0;
        while (first[len] && len < sizeof(head) - 1) ++len;
        ob_copy(head, first, len);
        head[len] = 0;
        list[0] = head;
        ob_ee_set_exec_args(n, list);
    } else ob_ee_set_exec_args(n, list);
}

/* Reset both VIFs and leave CYCLE at cl = wl = 4, since programs may unpack
   before ever issuing STCYCL. The commands go through each VIF FIFO as one
   quadword. */
static void vif_init(void) {
    static const uint32_t commands[4] __attribute__((aligned(16))) = {
        0x01000404u, /* STCYCL cl 4, wl 4 */
        0x05000000u, /* STMOD 0 */
        0, 0};       /* NOP */
    static const uint32_t fifos[2] = {0xb0004000u, 0xb0005000u};
    unsigned i;
    *(volatile uint32_t *)0xb0003810u = 1; /* VIF0_FBRST.RST */
    *(volatile uint32_t *)0xb0003c10u = 1; /* VIF1_FBRST.RST */
    for (i = 0; i < 2; ++i) {
        register uint32_t src __asm__("$4") = (uint32_t)(uintptr_t)commands;
        register uint32_t dst __asm__("$5") = fifos[i];
        /* lq $8, 0($4); sq $8, 0($5) (LLVM does not assemble R5900 LQ/SQ) */
        __asm__ volatile (".word 0x78880000\n\t.word 0x7ca80000" :: "r"(src), "r"(dst) : "$8", "memory");
    }
}

/* ExecPS2: forget threads, semaphores, handlers and alarms, then continue the
   caller as a fresh root thread at `entry`. Installed syscall patches persist. */
static int32_t set_gs_crt(uint32_t interlace, uint32_t mode, uint32_t ffmd);
static void exec_program(struct ob_ee_frame *f, uint32_t entry, uint32_t gp) {
    uint32_t status = f->status;
    reset_threads();
    /* Retail resets the video mode here, Chulip relies on it. */
    set_gs_crt(1, 2, 1);
    ob_zero(intc_handlers, sizeof(intc_handlers));
    ob_zero(intc_heads, sizeof(intc_heads));
    ob_zero(dmac_handlers, sizeof(dmac_handlers));
    ob_zero(dmac_heads, sizeof(dmac_heads));
    ob_zero(sbus_handlers, sizeof(sbus_handlers));
    ob_ee_alarm_init();
    ob_ee_deci2_init(); /* channels belonged to the previous program */
    vif_init();
    heap_end = 0;
    ob_zero(f, sizeof(*f));
    f->status = status;
    f->epc = entry;
    f->gpr[28][0] = gp;
    f->gpr[29][0] = 0x8007ff00u;
    f->gpr[4][0] = (uint32_t)exec_argc;
    {   /* a1 = argv, pointing into the kernel's copy of the strings. */
        static uint32_t argv[17];
        uint32_t offset = 0;
        int i;
        for (i = 0; i < exec_argc; ++i) {
            argv[i] = (uint32_t)(uintptr_t)(exec_args + offset);
            while (exec_args[offset]) ++offset;
            ++offset;
        }
        argv[exec_argc] = 0;
        f->gpr[5][0] = (uint32_t)(uintptr_t)argv;
    }
    ob_puts("[openbios:ee] exec entry="); ob_puthex(entry); ob_puts(" argc=");
    ob_puthex((uint32_t)exec_argc); ob_puts("\n");
}

void ob_ee_kernel_init(void) {
    reset_threads();
    vif_init();
    /* The SBUS interrupt starts enabled. */
    if (!(INTC_MASK & 2u)) INTC_MASK = 2u;
    ob_ee_alarm_init();
    ob_ee_deci2_init();
    ob_ee_sif_init();
}
void ob_ee_kernel_strict(int enabled) { strict_syscalls = enabled; }


void ob_ee_vectors_init(void) {
    static const uint32_t vectors[] = {0x000, 0x080, 0x180, 0x200};
    unsigned i;
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        volatile uint32_t *v = (volatile uint32_t *)(uintptr_t)(0xa0000000u + vectors[i]);
        /* Load an absolute address so cached/uncached vector aliases both work. */
        uint32_t handler = (uint32_t)(uintptr_t)ob_ee_exception_entry;
        v[0] = 0x3c1a0000u | (handler >> 16); /* lui k0,hi */
        v[1] = 0x375a0000u | (handler & 0xffff); /* ori k0,k0,lo */
        v[2] = 0x03400008u; /* jr k0 */
        v[3] = 0;
    }
    __asm__ volatile ("sync" ::: "memory");
}

static void set_result(struct ob_ee_frame *f, int32_t value) {
    f->gpr[2][0] = (uint32_t)value;
    f->gpr[2][1] = value < 0 ? UINT32_MAX : 0;
}

/* Resolve only ordinary EE RAM. Kernel/private RAM and hardware are excluded. */
void *ob_ee_user_pointer(uint32_t address, uint32_t size) {
    /* Scratchpad RAM: 16 KiB at 0x70000000. */
    if (address >= 0x70000000u && address < 0x70004000u)
        return ob_range(address - 0x70000000u, size, 0x4000u) ? (void *)(uintptr_t)address : 0;
    if ((address >= 0x20000000u && address < 0x22000000u) ||
        (address >= 0x30000000u && address < 0x32000000u)) address &= 0x01ffffffu;
    if (address >= 0x80000000u && address < 0xc0000000u) address &= 0x1fffffffu;
    /* Guest callbacks run on the kernel's exception stacks (0x58000-0x78000),
       so structures they build there are valid guest pointers too. */
    if (address >= 0x00058000u && address < 0x00078000u && ob_range(address - 0x00058000u, size, 0x20000u))
        return (void *)(uintptr_t)(address | 0xa0000000u);
    /* Programs own RAM from 0x80000 up. */
    if (address < 0x00080000u || !ob_range(address, size, 0x02000000u)) return 0;
    return (void *)(uintptr_t)(address | 0xa0000000u);
}
static int code_pointer(uint32_t address) {
    if (address >= 0x80000000u && address < 0xc0000000u) address &= 0x1fffffffu;
    return !(address & 3) && address >= 0x1000 && address < 0x02000000u;
}
static uint32_t entry_address(uint32_t number) {
    if (number >= sizeof(syscall_handlers) / sizeof(syscall_handlers[0])) return 0;
    if (syscall_handlers[number]) return syscall_handlers[number];
    switch (number) {
    case 0x18: case 0x1e: case 0xfc: case 0xfd:
        return (uint32_t)(uintptr_t)ob_ee_alarm_set_entry;
    case 0x19: case 0x1f: case 0xfe: case 0xff:
        return (uint32_t)(uintptr_t)ob_ee_alarm_release_entry;
    case 0x12c: return (uint32_t)(uintptr_t)ob_ee_alarm_irq_entry;
    case 0x08: return (uint32_t)(uintptr_t)ob_ee_resume_intr_entry;
    default: return 0;
    }
}
static int add_intc(int cause, uint32_t address, int position, uint32_t gp, uint32_t arg) {
    int i, *link;
    if (cause < 0 || cause >= 15 || !code_pointer(address)) return OB_EINVAL;
    if (position != 0 && position != -1) return OB_EINVAL;
    for (i = 0; i < 32; ++i) if (!intc_handlers[i].address) break;
    if (i == 32) return OB_ENOSPC;
    link = &intc_heads[cause];
    if (position == -1) while (*link) link = &intc_handlers[*link - 1].next;
    intc_handlers[i] = (struct intc_handler){address, gp, cause, *link, arg};
    *link = i + 1;
    return i + 1;
}
/* The remove calls return the handlers left on that cause: callers re-enable
   the interrupt they disabled only while others still use it. */
static int count_handlers(int id, const struct intc_handler *table) {
    int n = 0;
    for (; id; id = table[id - 1].next) ++n;
    return n;
}
static int remove_intc(int cause, int id) {
    int *link;
    if (cause < 0 || cause >= 15 || id < 1 || id > 32) return OB_EINVAL;
    link = &intc_heads[cause];
    while (*link && *link != id) link = &intc_handlers[*link - 1].next;
    if (!*link) return OB_EINVAL;
    *link = intc_handlers[id - 1].next;
    ob_zero(&intc_handlers[id - 1], sizeof(intc_handlers[0]));
    return count_handlers(intc_heads[cause], intc_handlers);
}
static int add_dmac(int cause, uint32_t address, int position, uint32_t gp, uint32_t arg) {
    int i, *link;
    if (cause < 0 || cause >= 10 || !code_pointer(address) || position < -1) return -1;
    for (i = 0; i < 32; ++i) if (!dmac_handlers[i].address) break;
    if (i == 32) return -1;
    link = &dmac_heads[cause];
    if (position == -1) while (*link) link = &dmac_handlers[*link - 1].next;
    else if (position > 0) {
        while (*link && *link != position) link = &dmac_handlers[*link - 1].next;
        if (!*link) return -1;
    }
    dmac_handlers[i] = (struct intc_handler){address, gp, cause, *link, arg};
    *link = i + 1;
    return i + 1;
}
static int remove_dmac(int cause, int id) {
    int *link;
    if (cause < 0 || cause >= 10 || id < 1 || id > 32) return -1;
    link = &dmac_heads[cause];
    while (*link && *link != id) link = &dmac_handlers[*link - 1].next;
    if (!*link) return -1;
    *link = dmac_handlers[id - 1].next;
    ob_zero(&dmac_handlers[id - 1], sizeof(dmac_handlers[0]));
    return count_handlers(dmac_heads[cause], dmac_handlers);
}
/* R5900 CACHE index operations, encoded manually because the assembler targets
   MIPS II: CACHE op,0($t0). Index is address bits 6 and up; bit 0 selects the way. */
#define CACHE_LOOP(op, sets) do { \
    uint32_t line; \
    for (line = 0; line < (sets) * 64u; line += 64) { \
        __asm__ volatile ("move $8, %0\n\t.word %1\n\t.word %2" :: \
            "r"(line), "i"(0xbd000000u | (op) << 16), "i"(0xbd000001u | (op) << 16) : "$8", "memory"); \
    } \
} while (0)
static void flush_cache(uint32_t mode) {
    __asm__ volatile ("sync" ::: "memory");
    if (mode == 0) CACHE_LOOP(0x14, 64);       /* DXWBIN: 8 KiB data cache. */
    else if (mode & 1) CACHE_LOOP(0x16, 64);   /* DXIN: discard without writeback. */
    if (mode & 2) CACHE_LOOP(0x07, 128);       /* IXIN: 16 KiB instruction cache. */
    __asm__ volatile ("sync\n\t.word 0x0000040f" ::: "memory"); /* sync.l; sync.p */
}
/* GS privileged registers must be written with a single 64-bit store. */
static void gs_write64(uint32_t reg, uint64_t v) {
    uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32), t;
    __asm__ volatile (
        ".set push\n\t.set mips3\n\t"
        "dsll32 %1, %1, 0\n\t"
        "dsll32 %0, %2, 0\n\t"
        "dsrl32 %0, %0, 0\n\t"
        "or %0, %0, %1\n\t"
        "sd %0, 0(%3)\n\t"
        ".set pop"
        : "=&r"(t), "+r"(hi) : "r"(lo), "r"(0xb2000000u + reg) : "memory");
}
/* SetGsCrt. SYNCV's front porch differs for progressive output: the GS only
   alternates CSR.FIELD when it is odd, and interlaced programs pace on it. */
static int32_t set_gs_crt(uint32_t interlace, uint32_t mode, uint32_t ffmd) {
    static const struct {
        uint32_t mode; uint64_t smode1, srfsh, synch1, synch2, syncv_i, syncv_p;
    } modes[] = {
        {0x02, 0x0740814504ull, 8, 0x7f5b61f06f040ull, 0x33a4d8, 0xc7800601a01801ull, 0xc7800601a01802ull},
        {0x03, 0x0740816504ull, 8, 0x7f5c21fc83030ull, 0x3484bc, 0xa9000502101401ull, 0xa9000502101404ull},
        {0x50, 0x1740410504ull, 4, 0x402da2003482bull, 0x197a71, 0xc7800001e00009ull, 0xc7800001e00009ull},
        {0x53, 0x17404b0504ull, 0, 0, 0, 0, 0},
        {0x52, 0x13402204b2ull, 0, 0, 0, 0, 0},
        {0x51, 0x03402204b2ull, 0, 0, 0, 0, 0},
    };
    unsigned i;
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
        if (modes[i].mode == mode) break;
    if (i == sizeof(modes) / sizeof(modes[0])) {
        ob_puts("[openbios:ee] SetGsCrt unsupported mode="); ob_puthex(mode); ob_puts("\n");
        return 0;
    }
    gs_write64(0x20, (interlace & 1) | (ffmd & 1) << 1); /* SMODE2 */
    if (modes[i].srfsh) {
        gs_write64(0x30, modes[i].srfsh);
        gs_write64(0x40, modes[i].synch1);
        gs_write64(0x50, modes[i].synch2);
        gs_write64(0x60, (interlace & 1) ? modes[i].syncv_i : modes[i].syncv_p);
    } else {
        ob_puts("[openbios:ee] SetGsCrt: sync timings unknown for mode="); ob_puthex(mode); ob_puts("\n");
    }
    gs_write64(0x10, modes[i].smode1 | (3ull << 16)); /* PRST and SINT: hold the PLL */
    __asm__ volatile ("sync" ::: "memory");
    if (mode == 0x51 || mode == 0x52) {
        uint32_t start, now;
        gs_write64(0x10, (modes[i].smode1 | (1ull << 16)) & ~(1ull << 17));
        __asm__ volatile ("mfc0 %0, $9" : "=r"(start));
        do __asm__ volatile ("mfc0 %0, $9" : "=r"(now));
        while (now - start < 737280u);               /* 2.5 ms PLL settle (COP0 Count) */
    }
    gs_write64(0x10, (modes[i].smode1 | (1ull << 16)) & ~(1ull << 17)); /* release SINT */
    return 0;
}
static struct ob_ee_frame *schedule_frame(struct ob_ee_frame *f) {
    struct ob_ee_frame *next;
    if (scheduler.changed) ob_ee_fpu_flush(); /* the frame may be stored in a thread */
    next = ob_sched_finish(&scheduler, f);
    if (!next) {
        ob_puts("[openbios:ee] deadlock: no runnable threads\n");
        ob_halt();
    }
    return next;
}
struct ob_ee_frame *ob_ee_resume_interrupt(void) {
    struct ob_ee_frame *f = interrupt_frame;
    if (!f) {
        ob_puts("[openbios:ee] fatal exception: interrupt resume outside interrupt\n");
        ob_halt();
    }
    interrupt_frame = 0;
    ob_ee_exception_depth = 1; /* Abandon callback/nested syscall stacks. */
    return schedule_frame(f);
}
static struct ob_ee_frame *dispatch_interrupt(struct ob_ee_frame *f) {
    uint32_t pending = INTC_STAT & INTC_MASK;
    unsigned cause;
    interrupt_frame = f;
#ifdef OB_TRACE_SYSCALLS
    {
        static unsigned traced;
        if (traced < 256) {
            ++traced;
            ob_puts("[openbios:trace] irq-enter pending="); ob_puthex(pending);
            ob_puts(" dmac="); ob_puthex(DMAC_STAT); ob_puts(" epc="); ob_puthex(f->epc);
            ob_puts(" 12c="); ob_puthex(entry_address(0x12c));
            ob_puts(" iop-received="); ob_puthex(*(volatile uint32_t *)0xbc045004u);
            ob_puts(" iop-command="); ob_puthex(*(volatile uint32_t *)0xbc045008u);
            ob_puts(" iop-result="); ob_puthex(*(volatile uint32_t *)0xbc04500cu);
            ob_puts(" d5="); ob_puthex(*(volatile uint32_t *)0xb000c000u);
            ob_puts("/"); ob_puthex(*(volatile uint32_t *)0xb000c010u);
            ob_puts(" dctrl="); ob_puthex(*(volatile uint32_t *)0xb000e000u);
            ob_puts(" en="); ob_puthex(*(volatile uint32_t *)0xb000f520u);
            ob_puts(" smflg="); ob_puthex(SBUS_SMFLG);
            ob_puts("\n");
        }
    }
#endif
    if (DMAC_STAT & 0x8000u) {
        ob_puts("[openbios:ee] fatal DMA bus error\n");
        ob_halt();
    }
#ifdef OB_TRACE_SYSCALLS
    static uint32_t dmac_counts[16];
    extern uint32_t setdma_calls, setdma_zero;
    {   /* Interrupt counts per INTC cause and DMAC channel. */
        static uint32_t intc_counts[15], vblanks;
        unsigned k;
        for (k = 0; k < 15; ++k) if (pending & (1u << k)) ++intc_counts[k];
        if (++vblanks % 500 == 0) { /* every 500 interrupts of any cause */
            ob_puts("[openbios:irqs] intc");
            for (k = 0; k < 15; ++k) { ob_puts(" "); ob_puthex(intc_counts[k]); }
            ob_puts(" dmac");
            for (k = 0; k < 10; ++k) { ob_puts(" "); ob_puthex(dmac_counts[k]); }
            ob_puts(" mask="); ob_puthex(INTC_MASK); ob_puts(" dmask="); ob_puthex(DMAC_STAT >> 16);
            ob_puts(" setdma="); ob_puthex(setdma_calls); ob_puts("/"); ob_puthex(setdma_zero);
            ob_puts(" t0mode="); ob_puthex(*(volatile uint32_t *)0xb0000010u);
            ob_puts(" t0comp="); ob_puthex(*(volatile uint32_t *)0xb0000020u);
            ob_puts(" d5="); ob_puthex(*(volatile uint32_t *)0xb000c000u);
            ob_puts(" epc="); ob_puthex(f->epc); ob_puts("\n");
        }
    }
#endif
    for (cause = 0; cause < 15; ++cause) if (pending & (1u << cause)) {
        int id;
        INTC_STAT = 1u << cause; /* INTC status is write-one-to-clear. */
        if (cause == 12) {
            uint32_t handler = entry_address(0x12c);
            if (handler == (uint32_t)(uintptr_t)ob_ee_alarm_irq_entry) ob_ee_alarm_irq_entry();
            else ob_ee_call_guest(f->gpr[28][0], handler, cause, 0, 0, 0);
        }
        if (cause == 2 && vsync_flag) {
            uint32_t *flag = ob_ee_user_pointer(vsync_flag, 4);
            uint64_t *csr = ob_ee_user_pointer(vsync_csr, 8);
            if (flag) *flag = 1;
            if (csr) {
                ((uint32_t *)csr)[0] = *(volatile uint32_t *)0xb2001000u;
                ((uint32_t *)csr)[1] = *(volatile uint32_t *)0xb2001004u;
            }
            vsync_flag = 0;
        }
        if (cause == 1) {
            uint32_t flags = SBUS_SMFLG;
            unsigned bit;
            for (bit = 0; bit < 32; ++bit) if ((flags & (1u << bit)) && sbus_handlers[bit]) {
                SBUS_SMFLG = 1u << bit; /* Write-one clears the IOP's flag. */
                ob_ee_call_guest(f->gpr[28][0], sbus_handlers[bit], (int)bit, 0, 0, 0);
            }
        }
        for (id = intc_heads[cause]; id;) {
            struct intc_handler h = intc_handlers[id - 1];
            int result = ob_ee_call_guest(h.gp, h.address, cause, h.arg, f->epc, 0);
            id = h.next;
            if (result < 0) break;
        }
    }
    /* D_STAT low bits acknowledge completion; upper bits toggle masks.
       Write only the channel's status bit when dispatching. */
    pending = DMAC_STAT;
    pending = (pending & (pending >> 16)) & 0x3ffu;
    for (cause = 0; cause < 10; ++cause) if (pending & (1u << cause)) {
        int id;
        DMAC_STAT = 1u << cause;
#ifdef OB_TRACE_SYSCALLS
        ++dmac_counts[cause];
#endif
        if (cause == 6) ob_ee_sif_dma_irq(); /* Kernel SIF1 queue first. */
        for (id = dmac_heads[cause]; id;) {
            struct intc_handler h = dmac_handlers[id - 1];
            int result = ob_ee_call_guest(h.gp, h.address, cause, h.arg, f->epc, 0);
            id = h.next;
            if (result < 0) break;
        }
    }
    interrupt_frame = 0;
#ifdef OB_TRACE_SYSCALLS
    {
        static unsigned traced;
        struct ob_ee_frame *next;
        static unsigned samples;
        /* Sample the interrupted context every 64th interrupt (thread, PC, RA). */
        if ((++samples & 63) == 0 && samples < (1u << 22)) {
            ob_puts("[openbios:sample] t="); ob_puthex((uint32_t)scheduler.current);
            ob_puts(" epc="); ob_puthex(f->epc); ob_puts(" ra="); ob_puthex((uint32_t)f->gpr[31][0]);
            ob_puts("\n");
        }
        next = schedule_frame(f);
        if (traced < 256) {
            ++traced;
            ob_puts("[openbios:trace] irq intc="); ob_puthex(INTC_STAT);
            ob_puts(" dmac="); ob_puthex(DMAC_STAT); ob_puts(" epc="); ob_puthex(f->epc);
            ob_puts(" next="); ob_puthex(next->epc); ob_puts(" depth=");
            ob_puthex(ob_ee_exception_depth); ob_puts("\n");
        }
        return next;
    }
#endif
    return schedule_frame(f);
}
/* "dev:path" on a device other than the disc (e.g. host:), which programs
   load through the IOP's ioman; bare paths are ISO paths on the disc. */
static int on_other_device(const char *path) {
    unsigned n = 0;
    while (path[n] && path[n] != ':') ++n;
    return path[n] == ':' && !ob_equal(path, "cdrom", 5);
}

struct ob_ee_frame *ob_ee_exception(struct ob_ee_frame *f) {
    int32_t nr = (int32_t)f->gpr[3][0]; /* PS2 ABI uses v1, not MIPS Linux v0. */
    uint32_t a0 = f->gpr[4][0], a1 = f->gpr[5][0], a2 = f->gpr[6][0];
    uint32_t dispatch_number = nr < 0 ? 0u - (uint32_t)nr : (uint32_t)nr;
    void *ptr;
    if (((f->cause >> 2) & 31) == 0 && (f->cause & 0xc00)) {
        ob_ee_iop_log_drain(); /* IOP console text is forwarded on interrupts */
        return dispatch_interrupt(f);
    }
    if (((f->cause >> 2) & 31) != 8 || (f->cause & 0x80000000u)) {
        unsigned i, j;
        uint32_t badvaddr;
        __asm__ volatile ("mfc0 %0, $8" : "=r"(badvaddr));
        ob_puts("[openbios:ee] fatal exception cause="); ob_puthex(f->cause);
        ob_puts(" epc="); ob_puthex(f->epc); ob_puts(" ra="); ob_puthex(f->gpr[31][0]);
        ob_puts(" sp="); ob_puthex(f->gpr[29][0]); ob_puts(" badvaddr="); ob_puthex(badvaddr);
        ob_puts("\n");
        /* The IOP keeps running: pass on its console text for about a second. */
        for (i = 0; i < 100; ++i) {
            ob_ee_iop_log_drain();
            for (j = 0; j < 1000000u; ++j) __asm__ volatile ("nop");
        }
        ob_halt();
    }
    f->epc += 4;
#ifdef OB_TRACE_SYSCALLS
    {   /* Syscall trace with caller. */
        static unsigned traced;
        int intr_call = dispatch_number >= 0x10 && dispatch_number <= 0x1d; /* always shown */
        if ((traced < 200000 || intr_call) && nr != 0x74 && nr != 0x2b) {
            ++traced;
            ob_puts("[openbios:trace] syscall="); ob_puthex((uint32_t)nr);
            ob_puts(" epc="); ob_puthex(f->epc - 4); ob_puts(" ra="); ob_puthex(f->gpr[31][0]);
            ob_puts(" a0="); ob_puthex(a0); ob_puts(" a1="); ob_puthex(a1);
            ob_puts(" depth="); ob_puthex(ob_ee_exception_depth);
            ob_puts("\n");
        }
    }
#endif
    if (ob_ee_exception_depth > 1 &&
        (nr == 0x23 || nr == 0x24 || nr == 0x32 || nr == 0x44 || nr == 0x3c || nr == 0x3d)) {
        set_result(f, OB_EBUSY);
        return f;
    }
    if (dispatch_number < 256 && syscall_handlers[dispatch_number]) {
        set_result(f, ob_ee_call_guest_frame(f->gpr[28][0], syscall_handlers[dispatch_number], f));
        goto scheduled;
    }
    switch (nr) {
    case 0x7c:
        set_result(f, ob_ee_deci2_call((int32_t)a0, a1, f->gpr[28][0])); break;
    case 0x6b: ob_ee_sif_stop_dma(); set_result(f, 0); break;
    case 0x02: set_result(f, set_gs_crt(a0, a1, a2)); break; /* SetGsCrt */
    case 0x70: case -0x70: set_result(f, (int32_t)gs_imr); break; /* GsGetIMR */
    case 0x71: case -0x71: /* GsPutIMR: kept and written to the GS IMR register. */
        gs_imr = a0;
        *(volatile uint32_t *)0xb2001010u = a0;
        set_result(f, (int32_t)gs_imr); break; /* Returns the new value. */
    case 0x78: case -0x78:
        ob_ee_sif_set_dchain(); set_result(f, 0); break;
    case 0x77: case -0x77:
        set_result(f, (int32_t)ob_ee_sif_set_dma(a0, (int32_t)a1));
#ifdef OB_TRACE_SYSCALLS
        ++setdma_calls;
        if (!f->gpr[2][0]) ++setdma_zero;
#endif
        break;
    case 0x76: case -0x76: set_result(f, ob_ee_sif_dma_stat(a0)); break;
    case 0x79: set_result(f, ob_ee_sif_set_reg(a0, a1)); break;
    case 0x7a: set_result(f, ob_ee_sif_get_reg(a0)); break;
    case 0x5b: set_result(f, (int32_t)entry_address(a0)); break;
    case 0x64: case -0x68: /* FlushCache/iFlushCache: 0 WB D, 1 inval D, 2 inval I. */
        flush_cache(a0 & 3); set_result(f, 0); break;
    case 0x61: case 0x62: /* EnableCache/DisableCache: caches stay on; only write back. */
        flush_cache(0); set_result(f, 0); break;
    case 0x08:
        if (interrupt_frame) return ob_ee_resume_interrupt();
        set_result(f, OB_EINVAL); break;
    case 0x18: case -0x1e: case 0xfc: case -0xfd:
        if (!code_pointer(a1)) { set_result(f, OB_EINVAL); break; }
        set_result(f, ob_ee_alarm_set_entry(a0 & 0xffff, a1, a2)); break;
    case 0x19: case -0x1f: case 0xfe: case -0xff:
        set_result(f, ob_ee_alarm_release_entry((int32_t)a0)); break;
    case 0x10:
        set_result(f, add_intc((int)a0, a1, (int)a2, f->gpr[28][0], f->gpr[7][0])); break;
    case 0x11: set_result(f, remove_intc((int)a0, (int)a1)); break;
    case 0x12:
        set_result(f, add_dmac((int)a0, a1, (int)a2, f->gpr[28][0], f->gpr[7][0])); break;
    case 0x13: set_result(f, remove_dmac((int)a0, (int)a1)); break;
    case 0x16: case -0x1c:
    case 0x17: case -0x1d: {
        uint32_t bit, was_enabled;
        int enable = nr == 0x16 || nr == -0x1c;
        if (a0 >= 10) { set_result(f, -1); break; }
        bit = 1u << (a0 + 16);
        was_enabled = !!(DMAC_STAT & bit);
        if ((int)was_enabled != enable) DMAC_STAT = bit;
        if (a0 == 6) ob_ee_sif_dma_mask_changed(); /* Guest now owns the mask. */
        set_result(f, (int)was_enabled != enable); break;
    }
    case 0x55: case -0x55: { /* PutTLBEntry: first unwired slot that is invalid or matches. */
        int index = ob_ee_tlb_probe(a1);
        if (index < 0) {
            uint32_t e[4];
            for (index = 31; index < 48; ++index) {
                ob_ee_tlb_read((uint32_t)index, e);
                if (!(e[2] & 2u) && !(e[3] & 2u)) break;
            }
            if (index == 48) { set_result(f, -1); break; }
        }
        ob_ee_tlb_write((uint32_t)index, a0, a1, a2, f->gpr[7][0]);
        set_result(f, index); break;
    }
    case 0x56: case -0x56: /* _SetTLBEntry(index, mask, hi, lo0, lo1) */
        if (a0 >= 48) { set_result(f, -1); break; }
        ob_ee_tlb_write(a0, a1, a2, f->gpr[7][0], f->gpr[8][0]);
#ifdef OB_TRACE_SYSCALLS
        ob_puts("[openbios:tlb] "); ob_puthex(a0); ob_puts(" "); ob_puthex(a1); ob_puts(" ");
        ob_puthex(a2); ob_puts(" "); ob_puthex(f->gpr[7][0]); ob_puts(" "); ob_puthex(f->gpr[8][0]);
        ob_puts("\n");
#endif
        set_result(f, (int32_t)a0); break;
    case 0x57: case -0x57: { /* GetTLBEntry(index, out[4]) */
        uint32_t e[4];
        if (a0 >= 48 || !a1) { set_result(f, -1); break; }
        ob_ee_tlb_read(a0, e);
        ob_copy((void *)(uintptr_t)a1, e, sizeof(e));
        set_result(f, (int32_t)a0); break;
    }
    case 0x58: case -0x58: { /* ProbeTLBEntry(hi, &mask, &lo0, &lo1) */
        uint32_t e[4];
        int index = ob_ee_tlb_probe(a0);
        if (index >= 0) {
            ob_ee_tlb_read((uint32_t)index, e);
            if (a1) *(uint32_t *)(uintptr_t)a1 = e[0];
            if (a2) *(uint32_t *)(uintptr_t)a2 = e[2];
            if (f->gpr[7][0]) *(uint32_t *)(uintptr_t)f->gpr[7][0] = e[3];
        }
        set_result(f, index < 0 ? -1 : index); break;
    }
    case 0x82: set_result(f, ob_ee_tlb_init_standard()); break; /* _InitTLB */
    case 0x07: /* ExecPS2(entry, gp, argc, argv) */
        if (!code_pointer(a0)) { set_result(f, OB_EINVAL); break; }
        capture_user_args(0, (int)a2, f->gpr[7][0]);
        exec_program(f, a0, a1);
        goto scheduled;
    case 0x06: { /* LoadExecPS2(path, argc, argv) */
        char path[256];
        const char *name = exec_arg_pointer(a0, 1);
        uint32_t entry, n = 0;
        if (!name) { set_result(f, OB_EINVAL); break; }
        while (name[n] && n < sizeof(path) - 1) { path[n] = name[n]; ++n; }
        path[n] = 0;
        capture_user_args(path, (int)a1, a2);
        {   /* "path arg1 arg2": launch arguments appended to the path. */
            const char *list[16];
            int count = 0;
            for (n = 0; path[n] && path[n] != ' '; ++n) {}
            if (path[n] == ' ') {
                char *p = path + n;
                *p++ = 0;
                list[count++] = path;
                while (*p && count < 16) {
                    while (*p == ' ') *p++ = 0;
                    if (!*p) break;
                    list[count++] = p;
                    while (*p && *p != ' ') ++p;
                }
                ob_ee_set_exec_args(count, list);
            }
        }
        ob_puts("[openbios:ee] LoadExecPS2 "); ob_puts(path); ob_puts("\n");
        if (path[0] == 'r' && path[1] == 'o' && path[2] == 'm' && path[3] == '0' && path[4] == ':') {
            if (ob_ee_rom_load(path + 5, &entry)) {
                /* No such ROM program (e.g. the boot screen): boot the disc. */
                if (ob_ee_disc_boot(&entry)) { set_result(f, OB_ENOTFOUND); break; }
            }
        } else if (on_other_device(path) ? ob_ee_file_load(path, &entry) : ob_ee_disc_load(path, &entry)) {
            ob_puts("[openbios:ee] LoadExecPS2 failed\n");
            set_result(f, OB_ENOTFOUND); break;
        }
        exec_program(f, entry, 0);
        goto scheduled;
    }
    case 0x0d: /* SetVTLBRefillHandler(code 1-3, handler): returns the previous one */
    case 0x0e: { /* SetVCommonHandler(code, handler) */
        static uint32_t vhandlers[16];
        if (a0 >= 16) { set_result(f, 0); break; }
        set_result(f, (int32_t)vhandlers[a0]);
        vhandlers[a0] = a1;
        ob_puts("[openbios:ee] exception handler code="); ob_puthex(a0); ob_puts(" handler=");
        ob_puthex(a1); ob_puts(" (recorded, not called yet)\n");
        break;
    }
    case 0x73: /* SetVSyncFlag(flag, csr); v0 is left as the caller had it */
        vsync_flag = a0;
        vsync_csr = a1;
        break;
    case 0x0a: /* AddSbusIntcHandler */
        if (a0 >= 32 || sbus_handlers[a0] || !code_pointer(a1)) { set_result(f, -1); break; }
        sbus_handlers[a0] = a1;
        if (!(INTC_MASK & 2u)) INTC_MASK = 2u;
        set_result(f, (int32_t)a0); break;
    case 0x0b: /* RemoveSbusIntcHandler */
        if (a0 >= 32) { set_result(f, -1); break; }
        sbus_handlers[a0] = 0;
        set_result(f, (int32_t)a0); break;
    case 0x0c: /* Interrupt2Iop */
        if (a0 >= 32) { set_result(f, -1); break; }
        SBUS_MSFLG = 1u << a0;
        SBUS_CTRL = 1u << 18; /* Raise the IOP's SBUS interrupt. */
        set_result(f, (int32_t)a0); break;
    case 0x14: case -0x1a:
    case 0x15: case -0x1b: {
        uint32_t bit, was_enabled;
        int enable = nr == 0x14 || nr == -0x1a;
        if (a0 >= 15) { set_result(f, OB_EINVAL); break; }
        bit = 1u << a0;
        was_enabled = !!(INTC_MASK & bit);
        if ((int)was_enabled != enable) INTC_MASK = bit; /* Write-one toggles mask. */
        /* 1 only when this call changed the mask, as for EnableDmac: callers
           undo their EnableIntc only if it returned 1. */
        set_result(f, (int)was_enabled != enable); break;
    }
    case 0x74: /* Register a guest handler; zero restores the built-in service. */
        if (a0 >= sizeof(syscall_handlers) / sizeof(syscall_handlers[0]) ||
            (a1 && !code_pointer(a1))) {
            set_result(f, OB_EINVAL); break;
        }
        syscall_handlers[a0] = a1;
        ob_puts("[openbios:ee] SetSyscall number="); ob_puthex(a0);
        ob_puts(" handler="); ob_puthex(a1); ob_puts("\n");
        set_result(f, 0); break;
    case 0x3c: { /* SetupThread: gp, stack base, size, argument area, root return. */
        uint32_t top, stack = a1;
        struct ob_thread *root = &scheduler.threads[scheduler.current - 1];
        ptr = ob_ee_user_pointer(f->gpr[7][0], 324);
        /* Size 0 is allowed (Crash Twinsanity). */
        if (!ptr || a2 > 0x01e00000u) { set_result(f, OB_EINVAL); break; }
        if (stack == UINT32_MAX) stack = 0x02000000u - a2;
        if (a2 && !ob_ee_user_pointer(stack, a2)) { set_result(f, OB_EINVAL); break; }
        top = (stack + a2) & ~15u;
        ob_zero(ptr, 324);
        {   /* {argc, argv[16], strings[256]}; argv points into the caller's area. */
            uint32_t *words = ptr, offset = 0;
            int i;
            ob_copy((char *)ptr + 68, exec_args, sizeof(exec_args));
            words[0] = (uint32_t)exec_argc;
            for (i = 0; i < exec_argc; ++i) {
                words[1 + i] = f->gpr[7][0] + 68u + offset;
                while (exec_args[offset]) ++offset;
                ++offset;
            }
        }
        root->desc.gp = a0;
        root->desc.stack = stack;
        root->desc.stack_size = a2;
        root->desc.entry = f->epc;
        f->gpr[28][0] = a0;
        set_result(f, (int32_t)(top - 16)); break;
    }
    case 0x3d: { /* SetupHeap: returns start + size unclamped, or the root
                    stack's base for size -1. */
        uint32_t limit = scheduler.threads[0].desc.stack;
        if (!limit) limit = 0x01f00000u;
        heap_end = a1 == UINT32_MAX ? limit : a0 + a1;
        set_result(f, (int32_t)heap_end); break;
    }
    case 0x3e: set_result(f, (int32_t)heap_end); break;
    case 0x20: {
        struct ob_thread_desc d;
        ptr = ob_ee_user_pointer(a0, sizeof(d));
        if (!ptr) { set_result(f, OB_EINVAL); break; }
        ob_copy(&d, ptr, sizeof(d));
        if (!ob_ee_user_pointer(d.entry, 4) || !ob_ee_user_pointer(d.stack, d.stack_size)) {
            set_result(f, OB_EINVAL); break;
        }
        set_result(f, ob_thread_create(&scheduler, &d));
#ifdef OB_TRACE_SYSCALLS
        ob_puts("[openbios:trace] CreateThread entry="); ob_puthex(d.entry);
        ob_puts(" stack="); ob_puthex(d.stack); ob_puts("+"); ob_puthex(d.stack_size);
        ob_puts(" gp="); ob_puthex(d.gp); ob_puts(" prio=");
        ob_puthex((uint32_t)d.initial_priority); ob_puts(" caller prio=");
        ob_puthex((uint32_t)scheduler.threads[scheduler.current - 1].desc.current_priority); ob_puts("\n");
#endif
        break;
    }
    case 0x21: { /* DeleteThread returns the deleted thread's ID. */
        int result = ob_thread_delete(&scheduler, (int)a0);
        set_result(f, result ? result : (int)a0); break;
    }
    case 0x22: { /* StartThread: retail returns the thread ID, not 0 (probed on
                    SCPH-39001). R&C Size Matters checks it and deletes the
                    thread it just started otherwise. */
        int result = ob_thread_start(&scheduler, (int)a0, a1, f->status);
        set_result(f, result ? result : (int)a0); break;
    }
    case 0x23: set_result(f, ob_thread_exit(&scheduler, 0)); break;
    case 0x24: set_result(f, ob_thread_exit(&scheduler, 1)); break;
    case 0x25: case -0x26:
        set_result(f, ob_thread_terminate(&scheduler, (int)a0)); break;
    case 0x29: { /* ChangeThreadPriority */
        /* Returns the target's previous priority, or -1 on error. */
        struct ob_thread_status before;
        int result = ob_thread_status(&scheduler, a0 ? (int)a0 : scheduler.current, &before);
        if (!result) result = ob_thread_priority(&scheduler, (int)a0, (int)a1);
        set_result(f, result ? -1 : (int)before.desc.current_priority);
        break;
    }
    case -0x2a: { /* iChangeThreadPriority returns the previous priority. */
        struct ob_thread_status before;
        int result = ob_thread_status(&scheduler, a0 ? (int)a0 : scheduler.current, &before);
        if (!result) result = ob_thread_priority(&scheduler, (int)a0, (int)a1);
        set_result(f, result ? result : (int)before.desc.current_priority); break;
    }
    case 0x2b: case -0x2c:
        set_result(f, ob_thread_rotate(&scheduler, (int)a0)); break;
    case 0x2d: case -0x2e:
        set_result(f, ob_thread_release(&scheduler, (int)a0)); break;
    case 0x2f: case -0x2f: set_result(f, scheduler.current); break;
    case 0x30: case -0x31: {
        struct ob_thread_status out;
        int result;
        /* A NULL buffer just asks for the status (retail; MGS2 polls this way). */
        ptr = a1 ? ob_ee_user_pointer(a1, sizeof(out)) : 0;
        if (a1 && !ptr) { set_result(f, OB_EINVAL); break; }
        result = ob_thread_status(&scheduler, (int)a0, &out);
        if (!result) {
            /* Returns the status word; a free slot reads as all zero. */
            if (ptr) ob_copy(ptr, &out, sizeof(out));
            result = (int)out.desc.status;
        } else if ((int)a0 > 0 && (int)a0 <= OB_MAX_THREADS) {
            if (ptr) ob_zero(ptr, sizeof(out));
            result = 0;
        }
        set_result(f, result); break;
    }
    case 0x32: set_result(f, ob_thread_sleep(&scheduler)); break;
    case 0x35: case -0x36: set_result(f, ob_thread_cancel_wakeup(&scheduler, (int)a0)); break;
    case 0x37: case -0x38: set_result(f, ob_thread_suspend(&scheduler, (int)a0)); break;
    case 0x39: case -0x3a: set_result(f, ob_thread_resume(&scheduler, (int)a0)); break;
    case 0x33: case -0x34:
        set_result(f, ob_thread_wake(&scheduler, (int)a0)); break;
    case 0x40: {
        struct ob_sema_desc d;
        ptr = ob_ee_user_pointer(a0, sizeof(d));
        if (!ptr) { set_result(f, OB_EINVAL); break; }
        ob_copy(&d, ptr, sizeof(d));
        set_result(f, ob_sema_create(&scheduler, &d));
#ifdef OB_TRACE_SYSCALLS
        {
            const uint32_t *w = ptr;
            unsigned k;
            ob_puts("[openbios:trace] CreateSema desc=");
            for (k = 0; k < 6; ++k) { ob_puthex(w[k]); ob_puts(" "); }
            ob_puts("ret="); ob_puthex(f->gpr[2][0]); ob_puts("\n");
        }
#endif
        break;
    }
    case 0x41: case -0x49:
        set_result(f, ob_sema_delete(&scheduler, (int)a0)); break;
    case 0x42: case -0x43: set_result(f, ob_sema_signal(&scheduler, (int)a0)); break;
    case 0x44: set_result(f, ob_sema_wait(&scheduler, (int)a0, 0)); break;
    case 0x45: case -0x46: set_result(f, ob_sema_wait(&scheduler, (int)a0, 1)); break;
    case 0x47: case -0x48: {
        struct ob_sema_desc out;
        int result;
        ptr = ob_ee_user_pointer(a1, sizeof(out));
        if (!ptr) { set_result(f, OB_EINVAL); break; }
        result = ob_sema_status(&scheduler, (int)a0, &out);
        if (!result) {
            /* Returns the ID and leaves init_count as the caller had it. */
            struct ob_sema_desc *user = ptr;
            out.init_count = user->init_count;
            ob_copy(ptr, &out, sizeof(out));
            result = (int)a0;
        }
        set_result(f, result); break;
    }
    case 0x7f: set_result(f, 0x02000000); break; /* GetMemorySize */
    case 0x7e: set_result(f, 0); break; /* MachineType: consumer unit */
    case 0x4a: /* SetOsdConfigParam */
    case 0x4b:
        ptr = ob_ee_user_pointer(a0, 4);
        if (!ptr) { set_result(f, OB_EINVAL); break; }
        if (nr == 0x4a) ob_copy(&osd_config, ptr, 4);
        else ob_copy(ptr, &osd_config, 4);
        set_result(f, 0); break;
    case 0x6e: /* Set/GetOsdConfigParam2 */
    case 0x6f:
        ptr = ob_ee_user_pointer(a0, a1);
        if (!ptr || !ob_range(a2, a1, sizeof(osd_extended))) {
            set_result(f, OB_EINVAL); break;
        }
        if (nr == 0x6e) ob_copy(osd_extended + a2, ptr, a1);
        else ob_copy(ptr, osd_extended + a2, a1);
        set_result(f, 0); break;
    default:
        ob_puts("[openbios:ee] unsupported syscall="); ob_puthex((uint32_t)nr);
        ob_puts(" epc="); ob_puthex(f->epc - 4); ob_puts("\n");
        ob_puts("[openbios:ee] arguments="); ob_puthex(a0); ob_puts(" ");
        ob_puthex(a1); ob_puts(" "); ob_puthex(a2); ob_puts("\n");
        if (strict_syscalls) ob_halt();
        set_result(f, OB_ENOSYS);
        break;
    }
#ifdef OB_TRACE_SYSCALLS
    if (nr != 0x74) {
        ob_puts("[openbios:trace] ret="); ob_puthex((uint32_t)f->gpr[2][0]);
        if (nr == 0x30 || nr == -0x31) {
            const uint32_t *st = ob_ee_user_pointer(a1, 28);
            if (st) { ob_puts(" status="); ob_puthex(st[0]); ob_puts(" prio="); ob_puthex(st[6]); }
        }
        ob_puts(" cur="); ob_puthex((uint32_t)scheduler.current); ob_puts("\n");
    }
#endif
scheduled:
    return ob_ee_exception_depth > 1 ? f : schedule_frame(f);
}

/* Minimal EELOAD at 0x82000, entered with argc in a0 and argv in a1. It runs
   argv[1] if given, otherwise "rom0:OSDSYS", through LoadExecPS2. The layout
   follows the EELOAD interface that fast-boot loaders patch: a JAL to main at
   0x9c, the exec step at 0x2b8 marked by a JAL word at 0x600, and the default
   path as an 8-byte aligned string. A loader may replace the path and append
   arguments after a space, which LoadExecPS2 splits off. */
#define EELOAD_ADDRESS 0x00082000u
#define EELOAD_PATH 0x680u
void ob_ee_install_eeload(void) {
    static const uint32_t main_code[] = {
        0x28880002u, /* 100: slti t0, a0, 2 */
        0x15000004u, /* 104: bnez t0, default */
        0x00000000u,
        0x8ca40004u, /* 10c: lw a0, 4(a1): argv[1] */
        0x10000069u, /* 110: b exec */
        0x00002821u, /* 114: move a1, zero */
        0x3c040008u, /* 118: default: lui a0, 0x0008 */
        0x34840000u | (EELOAD_PATH + 0x2000u), /* 11c: ori a0, a0, path */
        0x10000064u, /* 120: b exec */
        0x00002821u, /* 124: move a1, zero */
    };
    static const uint32_t exec_code[] = {
        0x00003021u, /* 2b8: exec: move a2, zero */
        0x24030006u, /* li v1, 6 (LoadExecPS2) */
        0x0000000cu, /* syscall */
        0x1000ffffu, /* b . */
        0x00000000u,
    };
    static const char path[] = "rom0:OSDSYS";
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)(0xa0000000u | EELOAD_ADDRESS);
    unsigned i;
    for (i = 0; i < 0xc00 / 4; ++i) w[i] = 0;
    w[0x9c / 4] = 0x0c000000u | ((EELOAD_ADDRESS + 0x100) >> 2); /* jal main */
    w[0xa4 / 4] = 0x1000ffffu;                                    /* b . */
    for (i = 0; i < sizeof(main_code) / 4; ++i) w[0x100 / 4 + i] = main_code[i];
    for (i = 0; i < sizeof(exec_code) / 4; ++i) w[0x2b8 / 4 + i] = exec_code[i];
    w[0x600 / 4] = 0x0c000000u | ((EELOAD_ADDRESS + 0x2b8) >> 2); /* marker, not executed */
    ob_copy((void *)(uintptr_t)(0xa0000000u | (EELOAD_ADDRESS + EELOAD_PATH)), path, sizeof(path));
    flush_cache(0);
    flush_cache(2);
}
void ob_ee_run_eeload(void) {
    ((void (*)(int, uint32_t))(uintptr_t)EELOAD_ADDRESS)(0, 0);
    for (;;) {} /* EELOAD never returns: LoadExecPS2 replaces the program */
}
