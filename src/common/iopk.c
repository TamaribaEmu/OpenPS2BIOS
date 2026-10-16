/* SPDX-License-Identifier: MIT */
#include "openbios/iopk.h"

void obk_init(struct obk_kernel *k) {
    ob_zero(k, sizeof(*k));
}
struct obk_thread *obk_thread(struct obk_kernel *k, int id) {
    if (id < 1 || id > OBK_THREADS || !k->threads[id - 1].used) return 0;
    return &k->threads[id - 1];
}
static struct obk_sema *sema(struct obk_kernel *k, int id) {
    if (id < 1 || id > OBK_SEMAS || !k->semas[id - 1].used) return 0;
    return &k->semas[id - 1];
}
static struct obk_event *event(struct obk_kernel *k, int id) {
    if (id < 1 || id > OBK_EVENTS || !k->events[id - 1].used) return 0;
    return &k->events[id - 1];
}
static void make_ready(struct obk_kernel *k, struct obk_thread *t, int32_t result) {
    t->status = OBK_THS_READY;
    t->wait_type = 0;
    t->wait_id = 0;
    t->order = ++k->order;
    t->context.r[2] = (uint32_t)result;
}
static void block_current(struct obk_kernel *k, int type, int id) {
    struct obk_thread *t = obk_thread(k, k->current);
    t->status = OBK_THS_WAIT;
    t->wait_type = type;
    t->wait_id = id;
    t->order = ++k->order;
}
static int valid_priority(int p) { return p >= OBK_PRIORITY_MIN && p <= OBK_PRIORITY_MAX; }

int obk_adopt_current(struct obk_kernel *k, int priority) {
    struct obk_thread *t = &k->threads[0];
    if (t->used) return OBK_ILLEGAL_THID;
    t->used = 1;
    t->status = OBK_THS_RUN;
    t->priority = t->init_priority = priority;
    t->order = ++k->order;
    k->current = 1;
    return 1;
}
int obk_thread_create(struct obk_kernel *k, uint32_t attr, uint32_t option, uint32_t entry,
                      uint32_t stack, uint32_t stack_size, int priority, uint32_t gp) {
    unsigned i;
    if (!entry || (entry & 3)) return OBK_ILLEGAL_ENTRY;
    if (!valid_priority(priority)) return OBK_ILLEGAL_PRIORITY;
    if (stack_size < 0x130) return OBK_ILLEGAL_STACK_SIZE;
    for (i = 0; i < OBK_THREADS; ++i) if (!k->threads[i].used) {
        struct obk_thread *t = &k->threads[i];
        ob_zero(t, sizeof(*t));
        t->used = 1;
        t->status = OBK_THS_DORMANT;
        t->attr = attr; t->option = option; t->entry = entry;
        t->stack = stack; t->stack_size = stack_size;
        t->priority = t->init_priority = priority;
        t->gp = gp;
        return (int)i + 1;
    }
    return OBK_NO_MEMORY;
}
int obk_thread_delete(struct obk_kernel *k, int id) {
    struct obk_thread *t = obk_thread(k, id);
    if (id == k->current) return OBK_NOT_DORMANT;
    if (!t) return OBK_UNKNOWN_THID;
    if (t->status != OBK_THS_DORMANT) return OBK_NOT_DORMANT;
    ob_zero(t, sizeof(*t));
    return OBK_OK;
}
int obk_thread_start(struct obk_kernel *k, int id, uint32_t arg, uint32_t exit_ra) {
    struct obk_thread *t = obk_thread(k, id);
    if (!t) return OBK_UNKNOWN_THID;
    if (t->status != OBK_THS_DORMANT) return OBK_NOT_DORMANT;
    ob_zero(&t->context, sizeof(t->context));
    t->context.r[4] = arg;                                  /* a0 */
    t->context.r[28] = t->gp;                               /* gp */
    t->context.r[29] = (t->stack + t->stack_size) & ~7u;    /* sp: stack top */
    t->context.r[31] = exit_ra;                             /* ra */
    t->context.epc = t->entry;
    /* Kernel mode; interrupt mask IM2 (IOP INTC) and IEp so rfe enables IRQs. */
    t->context.sr = 0x00000404u;
    t->priority = t->init_priority;
    t->wakeups = 0;
    make_ready(k, t, 0);
    return OBK_OK;
}
int obk_thread_exit(struct obk_kernel *k) {
    struct obk_thread *t = obk_thread(k, k->current);
    t->status = OBK_THS_DORMANT;
    t->wait_type = 0;
    return OBK_BLOCK;
}
int obk_thread_terminate(struct obk_kernel *k, int id) {
    struct obk_thread *t = obk_thread(k, id);
    if (id == k->current) return OBK_ILLEGAL_THID;
    if (!t) return OBK_UNKNOWN_THID;
    if (t->status == OBK_THS_DORMANT) return OBK_DORMANT;
    t->status = OBK_THS_DORMANT;
    t->wait_type = 0;
    t->wait_id = 0;
    return OBK_OK;
}
int obk_thread_priority(struct obk_kernel *k, int id, int priority) {
    struct obk_thread *t = obk_thread(k, id ? id : k->current);
    if (!t) return OBK_UNKNOWN_THID;
    if (priority == 0) priority = t->init_priority;
    if (!valid_priority(priority)) return OBK_ILLEGAL_PRIORITY;
    if (t->status == OBK_THS_DORMANT) return OBK_DORMANT;
    t->priority = priority;
    if (t->status == OBK_THS_READY) t->order = ++k->order;
    return OBK_OK;
}
int obk_thread_rotate(struct obk_kernel *k, int priority) {
    struct obk_thread *cur = obk_thread(k, k->current);
    if (priority == 0 && cur) priority = cur->priority;
    if (!valid_priority(priority) && priority != OBK_PRIORITY_IDLE) return OBK_ILLEGAL_PRIORITY;
    /* Move the first thread of that priority (the runner if it has it) to the end. */
    if (cur && cur->priority == priority) cur->order = ++k->order;
    else {
        struct obk_thread *first = 0;
        unsigned i;
        for (i = 0; i < OBK_THREADS; ++i) {
            struct obk_thread *t = &k->threads[i];
            if (t->used && t->status == OBK_THS_READY && t->priority == priority &&
                (!first || t->order < first->order)) first = t;
        }
        if (first) first->order = ++k->order;
    }
    return OBK_OK;
}
int obk_thread_release_wait(struct obk_kernel *k, int id) {
    struct obk_thread *t = obk_thread(k, id);
    if (!t) return OBK_UNKNOWN_THID;
    if (t->status != OBK_THS_WAIT) return OBK_NOT_WAIT;
    make_ready(k, t, OBK_RELEASE_WAIT);
    return OBK_OK;
}
int obk_sleep(struct obk_kernel *k) {
    struct obk_thread *t = obk_thread(k, k->current);
    if (t->wakeups > 0) { --t->wakeups; return OBK_OK; }
    block_current(k, OBK_WAIT_SLEEP, 0);
    return OBK_BLOCK;
}
int obk_wakeup(struct obk_kernel *k, int id) {
    struct obk_thread *t = obk_thread(k, id);
    if (!t) return OBK_UNKNOWN_THID;
    if (t->status == OBK_THS_DORMANT) return OBK_DORMANT;
    if (t->status == OBK_THS_WAIT && t->wait_type == OBK_WAIT_SLEEP) make_ready(k, t, OBK_OK);
    else ++t->wakeups;
    return OBK_OK;
}
int obk_cancel_wakeup(struct obk_kernel *k, int id) {
    struct obk_thread *t = obk_thread(k, id ? id : k->current);
    int previous;
    if (!t) return OBK_UNKNOWN_THID;
    previous = t->wakeups;
    t->wakeups = 0;
    return previous;
}
int obk_delay(struct obk_kernel *k, uint64_t cycles) {
    struct obk_thread *t = obk_thread(k, k->current);
    block_current(k, OBK_WAIT_DELAY, 0);
    t->wake_time = k->now + cycles;
    return OBK_BLOCK;
}
uint64_t obk_tick(struct obk_kernel *k, uint64_t now) {
    uint64_t next = UINT64_MAX;
    unsigned i;
    if (now > k->now) k->now = now;
    for (i = 0; i < OBK_THREADS; ++i) {
        struct obk_thread *t = &k->threads[i];
        if (!t->used || t->status != OBK_THS_WAIT || t->wait_type != OBK_WAIT_DELAY) continue;
        if (t->wake_time <= k->now) make_ready(k, t, OBK_OK);
        else if (t->wake_time - k->now < next) next = t->wake_time - k->now;
    }
    return next;
}

int obk_sema_create(struct obk_kernel *k, uint32_t attr, uint32_t option, int initial, int max) {
    unsigned i;
    /* Only the attribute is validated; counts are stored as given. */
    if (attr & ~0x101u) return OBK_ILLEGAL_ATTR;
    for (i = 0; i < OBK_SEMAS; ++i) if (!k->semas[i].used) {
        struct obk_sema *s = &k->semas[i];
        s->used = 1; s->attr = attr; s->option = option;
        s->count = initial; s->initial = initial; s->max = max;
        return (int)i + 1;
    }
    return OBK_NO_MEMORY;
}
static struct obk_thread *first_waiter(struct obk_kernel *k, int type, int id, int by_priority) {
    struct obk_thread *best = 0;
    unsigned i;
    for (i = 0; i < OBK_THREADS; ++i) {
        struct obk_thread *t = &k->threads[i];
        if (!t->used || t->status != OBK_THS_WAIT || t->wait_type != type || t->wait_id != id) continue;
        if (!best || (by_priority && t->priority < best->priority) ||
            ((!by_priority || t->priority == best->priority) && t->order < best->order)) best = t;
    }
    return best;
}
static int count_waiters(struct obk_kernel *k, int type, int id) {
    unsigned i;
    int n = 0;
    for (i = 0; i < OBK_THREADS; ++i) {
        struct obk_thread *t = &k->threads[i];
        if (t->used && t->status == OBK_THS_WAIT && t->wait_type == type && t->wait_id == id) ++n;
    }
    return n;
}
static void release_all(struct obk_kernel *k, int type, int id) {
    struct obk_thread *t;
    while ((t = first_waiter(k, type, id, 0))) make_ready(k, t, OBK_WAIT_DELETE);
}
int obk_sema_delete(struct obk_kernel *k, int id) {
    struct obk_sema *s = sema(k, id);
    if (!s) return OBK_UNKNOWN_SEMID;
    release_all(k, OBK_WAIT_SEMA, id);
    ob_zero(s, sizeof(*s));
    return OBK_OK;
}
int obk_sema_signal(struct obk_kernel *k, int id) {
    struct obk_sema *s = sema(k, id);
    struct obk_thread *t;
    if (!s) return OBK_UNKNOWN_SEMID;
    t = first_waiter(k, OBK_WAIT_SEMA, id, (s->attr & OBK_SA_THPRI) != 0);
    if (t) { make_ready(k, t, OBK_OK); return OBK_OK; }
    if (s->count >= s->max) return OBK_SEMA_OVF;
    ++s->count;
    return OBK_OK;
}
int obk_sema_wait(struct obk_kernel *k, int id, int poll) {
    struct obk_sema *s = sema(k, id);
    if (!s) return OBK_UNKNOWN_SEMID;
    if (s->count > 0) { --s->count; return OBK_OK; }
    if (poll) return OBK_SEMA_ZERO;
    block_current(k, OBK_WAIT_SEMA, id);
    return OBK_BLOCK;
}
int obk_sema_status(struct obk_kernel *k, int id, int *count, int *max, int *initial,
                    int *waiting, uint32_t *attr, uint32_t *option) {
    struct obk_sema *s = sema(k, id);
    if (!s) return OBK_UNKNOWN_SEMID;
    *count = s->count; *max = s->max; *initial = s->initial;
    *waiting = count_waiters(k, OBK_WAIT_SEMA, id);
    *attr = s->attr; *option = s->option;
    return OBK_OK;
}

int obk_event_create(struct obk_kernel *k, uint32_t attr, uint32_t option, uint32_t bits) {
    unsigned i;
    if (attr & ~(uint32_t)OBK_EA_MULTI) return OBK_ILLEGAL_ATTR;
    for (i = 0; i < OBK_EVENTS; ++i) if (!k->events[i].used) {
        struct obk_event *e = &k->events[i];
        e->used = 1; e->attr = attr; e->option = option; e->bits = e->initial = bits;
        return (int)i + 1;
    }
    return OBK_NO_MEMORY;
}
int obk_event_delete(struct obk_kernel *k, int id) {
    struct obk_event *e = event(k, id);
    if (!e) return OBK_UNKNOWN_EVFID;
    release_all(k, OBK_WAIT_EVENT, id);
    ob_zero(e, sizeof(*e));
    return OBK_OK;
}
static int satisfied(uint32_t have, uint32_t want, uint32_t mode) {
    return (mode & OBK_WEF_OR) ? (have & want) != 0 : (have & want) == want;
}
int obk_event_set(struct obk_kernel *k, int id, uint32_t bits) {
    struct obk_event *e = event(k, id);
    unsigned i;
    if (!e) return OBK_UNKNOWN_EVFID;
    e->bits |= bits;
    /* Wake satisfied waiters in queue order; a clearing waiter may stop the rest. */
    for (;;) {
        struct obk_thread *best = 0;
        for (i = 0; i < OBK_THREADS; ++i) {
            struct obk_thread *t = &k->threads[i];
            if (t->used && t->status == OBK_THS_WAIT && t->wait_type == OBK_WAIT_EVENT &&
                t->wait_id == id && satisfied(e->bits, t->event_bits, t->event_mode) &&
                (!best || t->order < best->order)) best = t;
        }
        if (!best) break;
        best->event_result = e->bits;
        if (best->event_mode & OBK_WEF_CLEAR) e->bits = 0;
        make_ready(k, best, OBK_OK);
    }
    return OBK_OK;
}
int obk_event_clear(struct obk_kernel *k, int id, uint32_t bits) {
    struct obk_event *e = event(k, id);
    if (!e) return OBK_UNKNOWN_EVFID;
    e->bits &= bits;
    return OBK_OK;
}
int obk_event_wait(struct obk_kernel *k, int id, uint32_t bits, uint32_t mode, int poll,
                   uint32_t *result) {
    struct obk_event *e = event(k, id);
    struct obk_thread *t;
    if (!e) return OBK_UNKNOWN_EVFID;
    if (!bits) return OBK_EVF_ILPAT;
    if (!(e->attr & OBK_EA_MULTI) && count_waiters(k, OBK_WAIT_EVENT, id)) return OBK_EVF_MULTI;
    if (satisfied(e->bits, bits, mode)) {
        if (result) *result = e->bits;
        if (mode & OBK_WEF_CLEAR) e->bits = 0;
        return OBK_OK;
    }
    if (poll) return OBK_EVF_COND;
    block_current(k, OBK_WAIT_EVENT, id);
    t = obk_thread(k, k->current);
    t->event_bits = bits;
    t->event_mode = mode;
    return OBK_BLOCK;
}
int obk_event_status(struct obk_kernel *k, int id, uint32_t *bits, uint32_t *initial,
                     int *waiting, uint32_t *attr, uint32_t *option) {
    struct obk_event *e = event(k, id);
    if (!e) return OBK_UNKNOWN_EVFID;
    *bits = e->bits; *initial = e->initial;
    *waiting = count_waiters(k, OBK_WAIT_EVENT, id);
    *attr = e->attr; *option = e->option;
    return OBK_OK;
}

/* ---- Fixed-size pools: free blocks are linked through their first word. ---- */
#define FPL_UNIT(size) (((uint32_t)(size) + sizeof(void *) - 1) & ~(uint32_t)(sizeof(void *) - 1))
#define POOL_MAX 0x200000u /* IOP RAM */
static struct obk_fpl *fpl(struct obk_kernel *k, int id) {
    if (id < 1 || id > OBK_FPLS || !k->fpls[id - 1].used) return 0;
    return &k->fpls[id - 1];
}
static int by_priority(uint32_t attr) { return (attr & OBK_TA_THPRI) != 0; }
int obk_fpl_bytes(uint32_t attr, int block_size, int blocks) {
    if (attr & ~(uint32_t)(OBK_TA_THPRI | OBK_PA_MEMBTM)) return OBK_ILLEGAL_ATTR;
    if (block_size <= 0 || blocks <= 0) return OBK_ILLEGAL_MEMSIZE;
    if ((uint64_t)FPL_UNIT(block_size) * (uint32_t)blocks > POOL_MAX) return OBK_NO_MEMORY;
    return (int)(FPL_UNIT(block_size) * (uint32_t)blocks);
}
int obk_fpl_create(struct obk_kernel *k, uint32_t attr, uint32_t option, int block_size, int blocks,
                   void *mem) {
    int r = obk_fpl_bytes(attr, block_size, blocks);
    unsigned i;
    if (r < 0) return r;
    if (!mem) return OBK_NO_MEMORY;
    for (i = 0; i < OBK_FPLS; ++i) if (!k->fpls[i].used) {
        struct obk_fpl *f = &k->fpls[i];
        uint32_t unit = FPL_UNIT(block_size), n;
        f->used = 1; f->attr = attr; f->option = option;
        f->block_size = (uint32_t)block_size; f->blocks = f->free_blocks = (uint32_t)blocks;
        f->mem = mem; f->free_list = 0;
        for (n = (uint32_t)blocks; n-- > 0;) {   /* Lowest address handed out first. */
            void **b = (void **)(void *)(f->mem + n * unit);
            *b = f->free_list;
            f->free_list = b;
        }
        return (int)i + 1;
    }
    return OBK_NO_MEMORY;
}
int obk_fpl_delete(struct obk_kernel *k, int id, void **mem) {
    struct obk_fpl *f = fpl(k, id);
    if (!f) return OBK_UNKNOWN_FPLID;
    release_all(k, OBK_WAIT_FPL, id);
    *mem = f->mem;
    ob_zero(f, sizeof(*f));
    return OBK_OK;
}
int obk_fpl_alloc(struct obk_kernel *k, int id, int poll, void **block) {
    struct obk_fpl *f = fpl(k, id);
    if (!f) return OBK_UNKNOWN_FPLID;
    if (f->free_list) {
        *block = f->free_list;
        f->free_list = *(void **)f->free_list;
        --f->free_blocks;
        return OBK_OK;
    }
    if (poll) return OBK_NO_MEMORY;
    block_current(k, OBK_WAIT_FPL, id);
    return OBK_BLOCK;
}
int obk_fpl_free(struct obk_kernel *k, int id, void *block) {
    struct obk_fpl *f = fpl(k, id);
    struct obk_thread *t;
    uint8_t *b = block;
    if (!f) return OBK_UNKNOWN_FPLID;
    if (b < f->mem || b >= f->mem + FPL_UNIT(f->block_size) * f->blocks ||
        (uint32_t)(b - f->mem) % FPL_UNIT(f->block_size)) return OBK_ILLEGAL_MEMBLOCK;
    t = first_waiter(k, OBK_WAIT_FPL, id, by_priority(f->attr));
    if (t) { t->wait_ptr = block; make_ready(k, t, OBK_OK); return OBK_OK; }
    *(void **)block = f->free_list;
    f->free_list = block;
    ++f->free_blocks;
    return OBK_OK;
}
int obk_fpl_status(struct obk_kernel *k, int id, uint32_t *attr, uint32_t *option, int *block_size,
                   int *blocks, int *free_blocks, int *waiting) {
    struct obk_fpl *f = fpl(k, id);
    if (!f) return OBK_UNKNOWN_FPLID;
    *attr = f->attr; *option = f->option;
    *block_size = (int)f->block_size; *blocks = (int)f->blocks; *free_blocks = (int)f->free_blocks;
    *waiting = count_waiters(k, OBK_WAIT_FPL, id);
    return OBK_OK;
}

/* ---- Heap chunks ---- */
#define CHUNK_MAGIC 0x4b4e4843u
#define BLK_FREE 0x45455246u
#define BLK_USED 0x44455355u
struct blk { uint32_t size, tag; }; /* size includes this header; multiple of 8 */
#define FIRST(c) ((struct blk *)(void *)((c) + 1))
#define END(c) ((uint8_t *)((c) + 1) + (c)->size)
#define NEXT(b) ((struct blk *)(void *)((uint8_t *)(b) + (b)->size))
static uintptr_t align8(void *p) { return ((uintptr_t)p + 7) & ~(uintptr_t)7; }
struct obk_chunk *obk_chunk_init(void *mem, uint32_t size) {
    uintptr_t at = align8(mem);
    uint32_t skip = (uint32_t)(at - (uintptr_t)mem);
    struct obk_chunk *c = (struct obk_chunk *)at;
    if (!mem || size < skip + OBK_CHUNK_OVERHEAD + 8) return 0;
    c->magic = CHUNK_MAGIC;
    c->size = ((size - skip) & ~7u) - (uint32_t)sizeof(*c);
    c->link.pad = 0;
    FIRST(c)->size = c->size;
    FIRST(c)->tag = BLK_FREE;
    return c;
}
struct obk_chunk *obk_chunk_at(void *mem) {
    struct obk_chunk *c = (struct obk_chunk *)align8(mem);
    return mem && c->magic == CHUNK_MAGIC ? c : 0;
}
void *obk_chunk_alloc(struct obk_chunk *c, uint32_t size) {
    struct blk *b;
    uint32_t need;
    if (!size || size > c->size) return 0;
    need = ((size + 7) & ~7u) + (uint32_t)sizeof(struct blk);
    for (b = FIRST(c); (uint8_t *)b < END(c) && b->size; b = NEXT(b)) {
        if (b->tag != BLK_FREE || b->size < need) continue;
        if (b->size - need >= 2 * sizeof(struct blk)) {
            struct blk *rest = (struct blk *)(void *)((uint8_t *)b + need);
            rest->size = b->size - need;
            rest->tag = BLK_FREE;
            b->size = need;
        }
        b->tag = BLK_USED;
        return b + 1;
    }
    return 0;
}
int obk_chunk_free(struct obk_chunk *c, void *block) {
    struct blk *b;
    for (b = FIRST(c); (uint8_t *)b < END(c) && b->size; b = NEXT(b)) {
        if ((void *)(b + 1) != block) continue;
        if (b->tag != BLK_USED) return -1;
        b->tag = BLK_FREE;
        for (b = FIRST(c); (uint8_t *)b < END(c) && b->size;) { /* Merge free neighbors. */
            struct blk *n = NEXT(b);
            if (b->tag == BLK_FREE && (uint8_t *)n < END(c) && n->tag == BLK_FREE) b->size += n->size;
            else b = n;
        }
        return 0;
    }
    return -1;
}
int obk_chunk_contains(struct obk_chunk *c, void *p) {
    return (uint8_t *)p >= (uint8_t *)(c + 1) && (uint8_t *)p < END(c);
}
int obk_chunk_empty(struct obk_chunk *c) {
    return FIRST(c)->tag == BLK_FREE && FIRST(c)->size == c->size;
}
static uint32_t chunk_scan(struct obk_chunk *c, int largest) {
    struct blk *b;
    uint32_t n = 0;
    for (b = FIRST(c); (uint8_t *)b < END(c) && b->size; b = NEXT(b)) {
        uint32_t room = b->size - (uint32_t)sizeof(*b);
        if (b->tag != BLK_FREE) continue;
        if (!largest) n += room;
        else if (room > n) n = room;
    }
    return n;
}
uint32_t obk_chunk_free_size(struct obk_chunk *c) { return chunk_scan(c, 0); }
uint32_t obk_chunk_largest(struct obk_chunk *c) { return chunk_scan(c, 1); }

/* ---- Variable-size pools ---- */
static struct obk_vpl *vpl(struct obk_kernel *k, int id) {
    if (id < 1 || id > OBK_VPLS || !k->vpls[id - 1].used) return 0;
    return &k->vpls[id - 1];
}
int obk_vpl_bytes(uint32_t attr, int size) {
    if (attr & ~(uint32_t)(OBK_TA_THPRI | OBK_PA_MEMBTM)) return OBK_ILLEGAL_ATTR;
    if (size <= 0) return OBK_ILLEGAL_MEMSIZE;
    if ((uint32_t)size > POOL_MAX) return OBK_NO_MEMORY;
    /* One allocation of the full size fits, plus alignment slack. */
    return (int)((((uint32_t)size + 7) & ~7u) + OBK_CHUNK_OVERHEAD + 8);
}
int obk_vpl_create(struct obk_kernel *k, uint32_t attr, uint32_t option, int size, void *mem) {
    int r = obk_vpl_bytes(attr, size);
    unsigned i;
    if (r < 0) return r;
    if (!mem) return OBK_NO_MEMORY;
    for (i = 0; i < OBK_VPLS; ++i) if (!k->vpls[i].used) {
        struct obk_vpl *v = &k->vpls[i];
        v->chunk = obk_chunk_init(mem, (uint32_t)r);
        v->used = 1; v->attr = attr; v->option = option; v->size = (uint32_t)size; v->mem = mem;
        return (int)i + 1;
    }
    return OBK_NO_MEMORY;
}
int obk_vpl_delete(struct obk_kernel *k, int id, void **mem) {
    struct obk_vpl *v = vpl(k, id);
    if (!v) return OBK_UNKNOWN_VPLID;
    release_all(k, OBK_WAIT_VPL, id);
    *mem = v->mem;
    ob_zero(v, sizeof(*v));
    return OBK_OK;
}
int obk_vpl_alloc(struct obk_kernel *k, int id, int size, int poll, void **block) {
    struct obk_vpl *v = vpl(k, id);
    void *p;
    if (!v) return OBK_UNKNOWN_VPLID;
    if (size <= 0 || (uint32_t)size > v->size) return OBK_ILLEGAL_MEMSIZE;
    p = obk_chunk_alloc(v->chunk, (uint32_t)size);
    if (p) { *block = p; return OBK_OK; }
    if (poll) return OBK_NO_MEMORY;
    block_current(k, OBK_WAIT_VPL, id);
    obk_thread(k, k->current)->wait_size = (uint32_t)size;
    return OBK_BLOCK;
}
int obk_vpl_free(struct obk_kernel *k, int id, void *block) {
    struct obk_vpl *v = vpl(k, id);
    struct obk_thread *t;
    if (!v) return OBK_UNKNOWN_VPLID;
    if (obk_chunk_free(v->chunk, block)) return OBK_ILLEGAL_MEMBLOCK;
    /* Serve waiters in queue order while the head of the queue fits. */
    while ((t = first_waiter(k, OBK_WAIT_VPL, id, by_priority(v->attr)))) {
        void *p = obk_chunk_alloc(v->chunk, t->wait_size);
        if (!p) break;
        t->wait_ptr = p;
        make_ready(k, t, OBK_OK);
    }
    return OBK_OK;
}
int obk_vpl_status(struct obk_kernel *k, int id, uint32_t *attr, uint32_t *option, int *size,
                   int *free_size, int *waiting) {
    struct obk_vpl *v = vpl(k, id);
    if (!v) return OBK_UNKNOWN_VPLID;
    *attr = v->attr; *option = v->option; *size = (int)v->size;
    *free_size = (int)obk_chunk_free_size(v->chunk);
    *waiting = count_waiters(k, OBK_WAIT_VPL, id);
    return OBK_OK;
}

/* ---- Message boxes ---- */
static struct obk_mbx *mbx(struct obk_kernel *k, int id) {
    if (id < 1 || id > OBK_MBXS || !k->mbxs[id - 1].used) return 0;
    return &k->mbxs[id - 1];
}
int obk_mbx_create(struct obk_kernel *k, uint32_t attr, uint32_t option) {
    unsigned i;
    if (attr & ~(uint32_t)(OBK_TA_THPRI | OBK_MBA_MSPRI)) return OBK_ILLEGAL_ATTR;
    for (i = 0; i < OBK_MBXS; ++i) if (!k->mbxs[i].used) {
        struct obk_mbx *m = &k->mbxs[i];
        ob_zero(m, sizeof(*m));
        m->used = 1; m->attr = attr; m->option = option;
        return (int)i + 1;
    }
    return OBK_NO_MEMORY;
}
int obk_mbx_delete(struct obk_kernel *k, int id) {
    struct obk_mbx *m = mbx(k, id);
    if (!m) return OBK_UNKNOWN_MBXID;
    release_all(k, OBK_WAIT_MBX, id);
    ob_zero(m, sizeof(*m));
    return OBK_OK;
}
/* Links are followed only while counted messages remain. A sender may reuse
   its packet once a receiver holds it, so the last message's link word is
   never read and appending writes only into the previous message. */
int obk_mbx_send(struct obk_kernel *k, int id, struct obk_message *msg) {
    struct obk_mbx *m = mbx(k, id);
    struct obk_message *prev = 0, *cur;
    struct obk_thread *t;
    int i;
    if (!m) return OBK_UNKNOWN_MBXID;
    t = first_waiter(k, OBK_WAIT_MBX, id, by_priority(m->attr));
    if (t) { t->wait_ptr = msg; make_ready(k, t, OBK_OK); return OBK_OK; }
    if (m->attr & OBK_MBA_MSPRI) {  /* After every message of equal or better priority. */
        for (i = 0, cur = m->head; i < m->count && cur->priority <= msg->priority; ++i) {
            prev = cur;
            if (i + 1 < m->count) cur = cur->next;
        }
        if (i < m->count) {          /* Insert before cur. */
            msg->next = cur;
            if (prev) prev->next = msg; else m->head = msg;
            ++m->count;
            return OBK_OK;
        }
    }
    if (m->count) m->tail->next = msg; else m->head = msg;
    m->tail = msg;
    ++m->count;
    return OBK_OK;
}
int obk_mbx_receive(struct obk_kernel *k, int id, int poll, struct obk_message **msg) {
    struct obk_mbx *m = mbx(k, id);
    if (!m) return OBK_UNKNOWN_MBXID;
    if (m->count > 0) {
        *msg = m->head;
        if (--m->count) m->head = m->head->next;
        else m->head = m->tail = 0;
        return OBK_OK;
    }
    if (poll) return OBK_MBOX_NOMSG;
    block_current(k, OBK_WAIT_MBX, id);
    return OBK_BLOCK;
}
int obk_mbx_status(struct obk_kernel *k, int id, uint32_t *attr, uint32_t *option, int *waiting,
                   int *count, struct obk_message **top) {
    struct obk_mbx *m = mbx(k, id);
    if (!m) return OBK_UNKNOWN_MBXID;
    *attr = m->attr; *option = m->option; *count = m->count; *top = m->count ? m->head : 0;
    *waiting = count_waiters(k, OBK_WAIT_MBX, id);
    return OBK_OK;
}

/* ---- Heaps: a header and first chunk in one block, extra chunks chained. ---- */
#define HEAP_MAGIC 0x50414548u
struct heap { uint32_t magic, flag, block_size, pad; union { struct obk_chunk *first; uint64_t pad; } c; };
static struct heap *heap_check(void *p) {
    struct heap *h = p;
    return h && h->magic == HEAP_MAGIC ? h : 0;
}
void *obk_heap_create(const struct obk_mem_ops *ops, int block_size, int flag) {
    uint32_t size = block_size > 0 ? (uint32_t)block_size : 0;
    struct heap *h;
    if (size < sizeof(*h) + OBK_CHUNK_OVERHEAD + 8) size = sizeof(*h) + OBK_CHUNK_OVERHEAD + 8;
    h = ops->alloc(size, (flag & OBK_HEAP_HIGH) != 0);
    if (!h) return 0;
    h->magic = HEAP_MAGIC;
    h->flag = (uint32_t)flag;
    h->block_size = size;
    h->pad = 0;
    h->c.pad = 0;
    h->c.first = obk_chunk_init(h + 1, size - (uint32_t)sizeof(*h));
    return h;
}
void obk_heap_delete(const struct obk_mem_ops *ops, void *heap) {
    struct heap *h = heap_check(heap);
    struct obk_chunk *c, *next;
    if (!h) return;
    for (c = h->c.first->link.next; c; c = next) { next = c->link.next; ops->free(c); }
    h->magic = 0;
    ops->free(h);
}
void *obk_heap_alloc(const struct obk_mem_ops *ops, void *heap, uint32_t size) {
    struct heap *h = heap_check(heap);
    struct obk_chunk *c, *last = 0;
    uint32_t grow;
    void *p;
    if (!h || !size || size > POOL_MAX) return 0;
    for (c = h->c.first; c; c = c->link.next) {
        if ((p = obk_chunk_alloc(c, size))) return p;
        last = c;
    }
    if (!(h->flag & OBK_HEAP_EXPAND)) return 0;
    grow = ((size + 7) & ~7u) + OBK_CHUNK_OVERHEAD;
    if (grow < h->block_size) grow = h->block_size;
    p = ops->alloc(grow, (h->flag & OBK_HEAP_HIGH) != 0);
    if (!p) return 0;
    if ((uintptr_t)p & 7 || !(c = obk_chunk_init(p, grow))) { ops->free(p); return 0; }
    last->link.next = c;
    return obk_chunk_alloc(c, size);
}
int obk_heap_free(const struct obk_mem_ops *ops, void *heap, void *p) {
    struct heap *h = heap_check(heap);
    struct obk_chunk *c, *prev = 0;
    if (!h) return -4;
    for (c = h->c.first; c; prev = c, c = c->link.next) {
        if (!obk_chunk_contains(c, p)) continue;
        if (obk_chunk_free(c, p)) return -1;
        if (prev && obk_chunk_empty(c)) { prev->link.next = c->link.next; ops->free(c); }
        return 0;
    }
    return -1;
}
int obk_heap_free_size(void *heap) {
    struct heap *h = heap_check(heap);
    struct obk_chunk *c;
    uint32_t n = 0;
    if (!h) return -4;
    for (c = h->c.first; c; c = c->link.next) n += obk_chunk_free_size(c);
    return (int)n;
}

int obk_pick(struct obk_kernel *k) {
    struct obk_thread *best = 0;
    int id = 0;
    unsigned i;
    for (i = 0; i < OBK_THREADS; ++i) {
        struct obk_thread *t = &k->threads[i];
        if (!t->used || (t->status != OBK_THS_READY && t->status != OBK_THS_RUN)) continue;
        if (!best || t->priority < best->priority ||
            (t->priority == best->priority && t->order < best->order)) {
            best = t;
            id = (int)i + 1;
        }
    }
    return id;
}
void obk_switch(struct obk_kernel *k, int id) {
    struct obk_thread *cur = obk_thread(k, k->current), *next = obk_thread(k, id);
    if (cur && cur->status == OBK_THS_RUN && cur != next) cur->status = OBK_THS_READY;
    if (next) next->status = OBK_THS_RUN;
    k->current = id;
}
