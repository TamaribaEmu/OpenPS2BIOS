/* SPDX-License-Identifier: MIT */
#include "openbios/sched.h"

static struct ob_thread *thread(struct ob_scheduler *s, int id) {
    if (id <= 0 || id > OB_MAX_THREADS || s->threads[id-1].state == OB_UNUSED) return 0;
    return &s->threads[id-1];
}
static struct ob_sema *sema(struct ob_scheduler *s, int id) {
    if (id <= 0 || id > OB_MAX_SEMAS || !s->semas[id-1].used) return 0;
    return &s->semas[id-1];
}
#ifdef OB_DEBUG_RING
/* Debug builds only: log thread wakeups to a ring in unused kernel RAM. */
#define DBG_RING ((volatile uint32_t *)0x80040000u)
static void dbg_wake(struct ob_scheduler *s, struct ob_thread *t, int32_t result) {
    uint32_t at = DBG_RING[0]++ & 1023;
    volatile uint32_t *e = DBG_RING + 4 + at * 6;
    e[0] = (uint32_t)(t - s->threads) + 1; e[1] = (uint32_t)t->wait_type; e[2] = (uint32_t)t->wait_id;
    e[3] = (uint32_t)result; e[4] = (uint32_t)s->current; e[5] = s->clock;
}
#endif
static void ready(struct ob_scheduler *s, struct ob_thread *t, int32_t result) {
#ifdef OB_DEBUG_RING
    dbg_wake(s, t, result);
#endif
    s->changed = 1;
    t->wait_type = OB_WAIT_NONE;
    t->wait_id = 0;
    t->state = OB_READY;
    t->order = ++s->clock;
    t->frame.gpr[2][0] = (uint32_t)result;
    t->frame.gpr[2][1] = result < 0 ? UINT32_MAX : 0;
}
void ob_sched_init(struct ob_scheduler *s, uint32_t exit_trampoline) {
    ob_zero(s, sizeof(*s));
    s->exit_trampoline = exit_trampoline;
    /* The boot executable is thread 1; its frame is captured at its first syscall. */
    s->current = 1;
    s->threads[0].state = OB_RUNNING;
    /* Programs start at priority 0. */
    s->threads[0].desc.initial_priority = 0;
    s->threads[0].desc.current_priority = 0;
    s->next_thread = 1;
    s->changed = 1;
}
int ob_thread_create(struct ob_scheduler *s, const struct ob_thread_desc *d) {
    s->changed = 1;
    int i, n;
    uint32_t stack_end;
    if (!d || !d->entry || (d->entry & 3) || d->stack < 0x00080000u ||
        d->stack_size < 256 || !ob_range(d->stack, d->stack_size, 0x02000000u) ||
        d->initial_priority < 0 || d->initial_priority > 128) /* attr/option are ignored. */
        return OB_EINVAL;
    stack_end = (d->stack + d->stack_size) & ~15u;
    if (stack_end < d->stack + 256) return OB_EINVAL;
    /* IDs are handed out round-robin so a freed ID is not reused at once. */
    for (n = 0; n < OB_MAX_THREADS; ++n) {
        struct ob_thread *t;
        i = (s->next_thread + n) % OB_MAX_THREADS;
        t = &s->threads[i];
        if (t->state != OB_UNUSED) continue;
        s->next_thread = (i + 1) % OB_MAX_THREADS;
        ob_zero(t, sizeof(*t));
        t->desc = *d;
        /* 128 is accepted but clamped to the lowest public level. */
        if (t->desc.initial_priority > 127) t->desc.initial_priority = 127;
        t->desc.current_priority = t->desc.initial_priority;
        t->state = OB_DORMANT;
        return i + 1;
    }
    return OB_ENOSPC;
}
int ob_thread_start(struct ob_scheduler *s, int id, uint32_t arg, uint32_t status) {
    s->changed = 1;
    struct ob_thread *t = thread(s, id);
    if (!t) return OB_EINVAL;
    if (t->state != OB_DORMANT) return OB_EBUSY;
    ob_zero(&t->frame, sizeof(t->frame));
    t->frame.epc = t->desc.entry;
    t->frame.status = status | 2; /* EXL stays set until ERET. */
    t->frame.gpr[4][0] = arg;
    t->frame.gpr[28][0] = t->desc.gp;
    t->frame.gpr[29][0] = ((t->desc.stack + t->desc.stack_size) & ~15u) - 16;
    t->frame.gpr[31][0] = s->exit_trampoline;
    t->desc.current_priority = t->desc.initial_priority;
    t->wakeups = 0;
    ready(s, t, 0);
    return OB_OK;
}
int ob_thread_delete(struct ob_scheduler *s, int id) {
    s->changed = 1;
    struct ob_thread *t = thread(s, id);
    if (!t) return OB_EINVAL;
    if (t->state != OB_DORMANT) return OB_EBUSY;
    ob_zero(t, sizeof(*t));
    return OB_OK;
}
int ob_thread_exit(struct ob_scheduler *s, int remove) {
    s->changed = 1;
    struct ob_thread *t = thread(s, s->current);
    if (!t) return OB_EINVAL;
    t->state = remove ? OB_UNUSED : OB_DORMANT;
    t->desc.current_priority = t->desc.initial_priority;
    t->wait_type = OB_WAIT_NONE;
    t->wait_id = 0;
    t->wakeups = 0;
    return OB_OK;
}
int ob_thread_sleep(struct ob_scheduler *s) {
    s->changed = 1;
    struct ob_thread *t = thread(s, s->current);
    if (!t) return OB_EINVAL;
    if (t->wakeups) { --t->wakeups; return OB_OK; }
    t->state = OB_WAITING;
    t->wait_type = OB_WAIT_SLEEP;
    return OB_OK;
}
int ob_thread_wake(struct ob_scheduler *s, int id) {
    s->changed = 1;
    struct ob_thread *t = thread(s, id);
    if (!t) return OB_EINVAL;
    if (t->state == OB_DORMANT || id == s->current) return OB_EBUSY;
    if (t->state == OB_WAITING && t->wait_type == OB_WAIT_SLEEP) ready(s, t, 0);
    else {
        if (t->wakeups == UINT32_MAX) return OB_ENOSPC;
        ++t->wakeups;
    }
    return OB_OK;
}
int ob_thread_priority(struct ob_scheduler *s, int id, int priority) {
    s->changed = 1;
    struct ob_thread *t = thread(s, id ? id : s->current);
    if (!t || priority < 0 || priority > 127) return OB_EINVAL;
    if (t->state == OB_DORMANT) return OB_EBUSY;
    t->desc.current_priority = priority;
    t->order = ++s->clock;
    return OB_OK;
}
int ob_thread_rotate(struct ob_scheduler *s, int priority) {
    s->changed = 1;
    struct ob_thread *oldest = 0;
    int i;
    if (priority < 0 || priority > 127) return OB_EINVAL;
    for (i = 0; i < OB_MAX_THREADS; ++i) {
        struct ob_thread *t = &s->threads[i];
        if ((t->state == OB_READY || t->state == OB_RUNNING) &&
            t->desc.current_priority == priority && (!oldest || t->order < oldest->order)) oldest = t;
    }
    if (oldest) oldest->order = ++s->clock;
    return OB_OK;
}
int ob_thread_status(struct ob_scheduler *s, int id, struct ob_thread_status *out) {
    struct ob_thread *t = thread(s, id ? id : s->current);
    if (!t || !out) return OB_EINVAL;
    out->desc = t->desc;
    switch (t->state) {
    case OB_RUNNING: out->desc.status = 1; break;
    case OB_READY: out->desc.status = t->suspended ? 8 : 2; break;
    case OB_WAITING: out->desc.status = t->suspended ? 12 : 4; break;
    default: out->desc.status = 16; break;
    }
    out->wait_type = t->wait_type;
    out->wait_id = (uint32_t)t->wait_id;
    out->wakeup_count = t->wakeups;
    return OB_OK;
}
/* A suspended ready thread is not scheduled; a suspended waiting thread
   (WAITSUSPEND) stays suspended after its wait ends. Both calls return the
   thread ID. Suspending the calling thread does not stop it, and resuming a
   thread that is not suspended succeeds. */
int ob_thread_suspend(struct ob_scheduler *s, int id) {
    s->changed = 1;
    int target = id ? id : s->current;
    struct ob_thread *t = thread(s, target);
    if (!t) return OB_EINVAL;
    if (t->state == OB_DORMANT) return OB_EBUSY;
    if (target != s->current) t->suspended = 1;
    return target;
}
int ob_thread_resume(struct ob_scheduler *s, int id) {
    s->changed = 1;
    struct ob_thread *t = thread(s, id);
    if (!t) return OB_EINVAL;
    if (t->suspended) {
        t->suspended = 0;
        if (t->state == OB_READY) t->order = ++s->clock;
    }
    return id;
}
int ob_thread_cancel_wakeup(struct ob_scheduler *s, int id) {
    struct ob_thread *t = thread(s, id ? id : s->current);
    int previous;
    if (!t) return OB_EINVAL;
    previous = (int)t->wakeups;
    t->wakeups = 0;
    return previous;
}
int ob_thread_release(struct ob_scheduler *s, int id) {
    s->changed = 1;
    struct ob_thread *t = thread(s, id);
    if (!t) return OB_EINVAL;
    if (t->state != OB_WAITING) return OB_EBUSY;
    ready(s, t, OB_EBUSY);
    return OB_OK;
}
int ob_thread_terminate(struct ob_scheduler *s, int id) {
    s->changed = 1;
    struct ob_thread *t = thread(s, id);
    if (!t) return OB_EINVAL;
    if (id == s->current || t->state == OB_DORMANT) return OB_EBUSY;
    t->state = OB_DORMANT;
    t->desc.current_priority = t->desc.initial_priority;
    t->wait_type = OB_WAIT_NONE;
    t->wait_id = 0;
    t->wakeups = 0;
    return OB_OK;
}
int ob_sema_create(struct ob_scheduler *s, const struct ob_sema_desc *d) {
    int i;
    /* Only a negative initial count is rejected; init > max is accepted. */
    int pick = -1;
    if (!d || d->init_count < 0) return OB_EINVAL;
    /* Reuse the most recently deleted ID first, then unused IDs in ascending
       order. */
    for (i = 0; i < OB_MAX_SEMAS; ++i) {
        if (s->semas[i].used) continue;
        if (pick < 0 || s->semas[i].freed > s->semas[pick].freed) pick = i;
    }
    if (pick < 0) return OB_ENOSPC;
    s->semas[pick].used = 1;
    s->semas[pick].desc = *d;
    s->semas[pick].desc.count = d->init_count;
    s->semas[pick].desc.wait_threads = 0;
    return pick + 1;
}
int ob_sema_delete(struct ob_scheduler *s, int id) {
    s->changed = 1;
    struct ob_sema *v = sema(s, id);
    int i;
    if (!v) return OB_EINVAL;
    for (i = 0; i < OB_MAX_THREADS; ++i) {
        struct ob_thread *t = &s->threads[i];
        if (t->state == OB_WAITING && t->wait_type == OB_WAIT_SEMA && t->wait_id == id)
            ready(s, t, OB_ENOTFOUND);
    }
    ob_zero(v, sizeof(*v));
    v->freed = ++s->sema_deletes;
    return id; /* Success returns the semaphore ID. */
}
int ob_sema_wait(struct ob_scheduler *s, int id, int poll) {
    s->changed = 1;
    struct ob_sema *v = sema(s, id);
    struct ob_thread *t = thread(s, s->current);
    if (!v || !t) return OB_EINVAL;
    if (v->desc.count > 0) { --v->desc.count; return id; }
    if (poll) return -1; /* PollSema on an empty semaphore. */
    t->state = OB_WAITING;
    t->wait_type = OB_WAIT_SEMA;
    t->wait_id = id;
    t->order = ++s->clock;
    return id; /* Replaced by the wake result if the wait is cancelled. */
}
int ob_sema_signal(struct ob_scheduler *s, int id) {
    s->changed = 1;
    struct ob_sema *v = sema(s, id);
    struct ob_thread *best = 0;
    int i;
    if (!v) return OB_EINVAL;
    for (i = 0; i < OB_MAX_THREADS; ++i) {
        struct ob_thread *t = &s->threads[i];
        if (t->state != OB_WAITING || t->wait_type != OB_WAIT_SEMA || t->wait_id != id) continue;
        if (!best || ((v->desc.attr & 1) && t->desc.current_priority < best->desc.current_priority) ||
            ((!(v->desc.attr & 1) || t->desc.current_priority == best->desc.current_priority) &&
                t->order < best->order)) best = t;
    }
    if (best) ready(s, best, id);
    else ++v->desc.count; /* Not bounded by max_count. */
    return id;
}
int ob_sema_status(struct ob_scheduler *s, int id, struct ob_sema_desc *out) {
    struct ob_sema *v = sema(s, id);
    int i;
    if (!v || !out) return OB_EINVAL;
    *out = v->desc;
    out->wait_threads = 0;
    for (i = 0; i < OB_MAX_THREADS; ++i) {
        struct ob_thread *t = &s->threads[i];
        if (t->state == OB_WAITING && t->wait_type == OB_WAIT_SEMA && t->wait_id == id)
            ++out->wait_threads;
    }
    return OB_OK;
}
struct ob_ee_frame *ob_sched_finish(struct ob_scheduler *s, const struct ob_ee_frame *f) {
    struct ob_thread *current = thread(s, s->current), *best = 0;
    int i, next = 0;
    /* Nothing changed since the last decision: the running thread continues. */
    if (!s->changed && f && current && current->state == OB_RUNNING && !current->suspended)
        return (struct ob_ee_frame *)(uintptr_t)f;
    s->changed = 0;
    if (current && current->state == OB_RUNNING) current->state = OB_READY;
    for (i = 0; i < OB_MAX_THREADS; ++i) {
        struct ob_thread *t = &s->threads[i];
        if (t->state == OB_READY && !t->suspended && (!best || t->desc.current_priority < best->desc.current_priority ||
            (t->desc.current_priority == best->desc.current_priority && t->order < best->order))) {
            best = t;
            next = i + 1;
        }
    }
    if (best && best == current && f) {
        /* Same thread continues: resume straight from the caller's frame. */
        best->state = OB_RUNNING;
        return (struct ob_ee_frame *)(uintptr_t)f;
    }
    if (current && f) ob_copy(&current->frame, f, sizeof(*f));
    if (!best) return 0;
    best->state = OB_RUNNING;
    s->current = next;
    return &best->frame;
}
