/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_SCHED_H
#define OPENBIOS_SCHED_H
#include "ee.h"
#define OB_MAX_THREADS 32
#define OB_MAX_SEMAS 256 /* EE kernel limit */
enum ob_thread_state { OB_UNUSED, OB_DORMANT, OB_READY, OB_RUNNING, OB_WAITING };
enum ob_wait_type { OB_WAIT_NONE, OB_WAIT_SLEEP, OB_WAIT_SEMA };
struct ob_thread_desc {
    uint32_t status, entry, stack, stack_size, gp;
    int32_t initial_priority, current_priority;
    uint32_t attr, option;
};
struct ob_thread_status {
    struct ob_thread_desc desc;
    uint32_t wait_type, wait_id, wakeup_count;
};
struct ob_sema_desc {
    int32_t count, max_count, init_count, wait_threads;
    uint32_t attr, option;
};
struct ob_thread {
    enum ob_thread_state state;
    struct ob_thread_desc desc;
    enum ob_wait_type wait_type;
    int wait_id;
    uint32_t wakeups;
    int suspended; /* Orthogonal to state: READY/WAITING threads may be suspended. */
    uint64_t order;
    struct ob_ee_frame frame;
};
struct ob_sema { int used; struct ob_sema_desc desc; uint32_t freed; /* delete sequence, 0 = never */ };
struct ob_scheduler {
    struct ob_thread threads[OB_MAX_THREADS];
    struct ob_sema semas[OB_MAX_SEMAS];
    int current;
    uint64_t clock;
    uint32_t exit_trampoline;
    int next_thread; /* Slot index where the next thread ID search starts. */
    int changed; /* Set by every state/priority change; lets ob_sched_finish skip its scan. */
    uint32_t sema_deletes; /* Orders freed semaphore slots for reuse. */
};
void ob_sched_init(struct ob_scheduler *s, uint32_t exit_trampoline);
int ob_thread_create(struct ob_scheduler *s, const struct ob_thread_desc *d);
int ob_thread_start(struct ob_scheduler *s, int id, uint32_t arg, uint32_t status);
int ob_thread_delete(struct ob_scheduler *s, int id);
int ob_thread_exit(struct ob_scheduler *s, int remove);
int ob_thread_sleep(struct ob_scheduler *s);
int ob_thread_wake(struct ob_scheduler *s, int id);
int ob_thread_priority(struct ob_scheduler *s, int id, int priority);
int ob_thread_rotate(struct ob_scheduler *s, int priority);
int ob_thread_status(struct ob_scheduler *s, int id, struct ob_thread_status *out);
int ob_thread_release(struct ob_scheduler *s, int id);
int ob_thread_terminate(struct ob_scheduler *s, int id);
int ob_thread_suspend(struct ob_scheduler *s, int id);
int ob_thread_resume(struct ob_scheduler *s, int id);
int ob_thread_cancel_wakeup(struct ob_scheduler *s, int id);
int ob_sema_create(struct ob_scheduler *s, const struct ob_sema_desc *d);
int ob_sema_delete(struct ob_scheduler *s, int id);
int ob_sema_wait(struct ob_scheduler *s, int id, int poll);
int ob_sema_signal(struct ob_scheduler *s, int id);
int ob_sema_status(struct ob_scheduler *s, int id, struct ob_sema_desc *out);
struct ob_ee_frame *ob_sched_finish(struct ob_scheduler *s, const struct ob_ee_frame *frame);
#endif
