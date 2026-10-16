/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_IOPK_H
#define OPENBIOS_IOPK_H
/* Portable IOP thread kernel core: threads, priorities, semaphores, event flags,
   sleep/wakeup, delays, fixed/variable memory pools, message boxes and heap
   chunks. No hardware access; the IOP glue saves contexts,
   performs the switch the core selects and drives the clock. */
#include "base.h"

#define OBK_THREADS 64
#define OBK_SEMAS 128
#define OBK_EVENTS 64
#define OBK_FPLS 32
#define OBK_VPLS 32
#define OBK_MBXS 64
#define OBK_PRIORITY_MIN 1
#define OBK_PRIORITY_MAX 126
#define OBK_PRIORITY_IDLE 127

/* Public IOP status and wait codes. */
#define OBK_THS_RUN 0x01
#define OBK_THS_READY 0x02
#define OBK_THS_WAIT 0x04
#define OBK_THS_DORMANT 0x10
#define OBK_WAIT_SLEEP 1
#define OBK_WAIT_DELAY 2
#define OBK_WAIT_SEMA 3
#define OBK_WAIT_EVENT 4
#define OBK_WAIT_MBX 5
#define OBK_WAIT_VPL 6
#define OBK_WAIT_FPL 7

/* Public IOP kernel error codes used here. */
#define OBK_OK 0
#define OBK_ILLEGAL_CONTEXT (-100)
#define OBK_NO_MEMORY (-400)
#define OBK_ILLEGAL_ATTR (-401)
#define OBK_ILLEGAL_ENTRY (-402)
#define OBK_ILLEGAL_PRIORITY (-403)
#define OBK_ILLEGAL_STACK_SIZE (-404)
#define OBK_ILLEGAL_THID (-406)
#define OBK_UNKNOWN_THID (-407)
#define OBK_UNKNOWN_SEMID (-408)
#define OBK_UNKNOWN_EVFID (-409)
#define OBK_UNKNOWN_MBXID (-410)
#define OBK_UNKNOWN_VPLID (-411)
#define OBK_UNKNOWN_FPLID (-412)
#define OBK_DORMANT (-413)
#define OBK_NOT_DORMANT (-414)
#define OBK_NOT_WAIT (-416)
#define OBK_RELEASE_WAIT (-418)
#define OBK_SEMA_ZERO (-419)
#define OBK_SEMA_OVF (-420)
#define OBK_EVF_COND (-421)
#define OBK_EVF_MULTI (-422)
#define OBK_EVF_ILPAT (-423)
#define OBK_MBOX_NOMSG (-424)
#define OBK_WAIT_DELETE (-425)
#define OBK_ILLEGAL_MEMBLOCK (-426)
#define OBK_ILLEGAL_MEMSIZE (-427)
/* Returned to the glue when the calling thread must block. */
#define OBK_BLOCK 1

#define OBK_SA_THPRI 0x001
#define OBK_EA_MULTI 0x002
#define OBK_WEF_OR 0x01
#define OBK_WEF_CLEAR 0x10
#define OBK_TA_THPRI 0x001   /* Wait queue in thread priority order (pools, mbx). */
#define OBK_PA_MEMBTM 0x200  /* Pool memory taken from the top of IOP memory. */
#define OBK_MBA_MSPRI 0x004  /* Messages ordered by their priority byte. */

/* Saved CPU state of a thread (R3000A). r[0] is unused. */
struct obk_context {
    uint32_t r[32];
    uint32_t hi, lo, epc, sr, cause;
};
struct obk_thread {
    int used, status, priority, init_priority;
    uint32_t attr, option, entry, stack, stack_size, gp;
    int wait_type, wait_id, wakeups;
    uint64_t wake_time;          /* OBK_WAIT_DELAY */
    uint32_t event_bits, event_mode, event_result; /* OBK_WAIT_EVENT */
    uint32_t wait_size;          /* OBK_WAIT_VPL request */
    void *wait_ptr;              /* Block or message handed over on wakeup */
    uint32_t order;              /* FIFO order within a priority / wait queue */
    struct obk_context context;
};
struct obk_sema { int used; uint32_t attr, option; int count, max, initial; };
struct obk_event { int used; uint32_t attr, option, bits, initial; };
/* Heap chunk: a header followed by 8-byte-headed blocks (first fit, coalescing). */
struct obk_chunk {
    uint32_t magic, size;                                /* size: bytes after header */
    union { struct obk_chunk *next; uint64_t pad; } link; /* Heap chain */
};
/* Caller-owned message, linked through its first word (iop_message_t). */
struct obk_message { struct obk_message *next; uint8_t priority, unused[3]; };
struct obk_fpl {
    int used; uint32_t attr, option, block_size, blocks, free_blocks;
    uint8_t *mem; void *free_list;
};
struct obk_vpl { int used; uint32_t attr, option, size; void *mem; struct obk_chunk *chunk; };
struct obk_mbx { int used; uint32_t attr, option; int count; struct obk_message *head, *tail; };
struct obk_kernel {
    struct obk_thread threads[OBK_THREADS];
    struct obk_sema semas[OBK_SEMAS];
    struct obk_event events[OBK_EVENTS];
    struct obk_fpl fpls[OBK_FPLS];
    struct obk_vpl vpls[OBK_VPLS];
    struct obk_mbx mbxs[OBK_MBXS];
    int current;      /* Running thread ID. */
    uint32_t order;
    uint64_t now;     /* System clock in IOP cycles. */
};

void obk_init(struct obk_kernel *k);
/* Turn the running boot context into thread 1 at the given priority. */
int obk_adopt_current(struct obk_kernel *k, int priority);
int obk_thread_create(struct obk_kernel *k, uint32_t attr, uint32_t option, uint32_t entry,
                      uint32_t stack, uint32_t stack_size, int priority, uint32_t gp);
int obk_thread_delete(struct obk_kernel *k, int id);
/* Prepare a dormant thread's context and make it ready. sp/ra are supplied by
   the glue (stack top and exit trampoline). */
int obk_thread_start(struct obk_kernel *k, int id, uint32_t arg, uint32_t exit_ra);
int obk_thread_exit(struct obk_kernel *k);               /* -> OBK_BLOCK */
int obk_thread_terminate(struct obk_kernel *k, int id);
int obk_thread_priority(struct obk_kernel *k, int id, int priority);
int obk_thread_rotate(struct obk_kernel *k, int priority);
int obk_thread_release_wait(struct obk_kernel *k, int id);
int obk_sleep(struct obk_kernel *k);                     /* OK or OBK_BLOCK */
int obk_wakeup(struct obk_kernel *k, int id);
int obk_cancel_wakeup(struct obk_kernel *k, int id);     /* previous count */
int obk_delay(struct obk_kernel *k, uint64_t cycles);    /* OBK_BLOCK */
/* Advance the clock and wake expired delays. Returns the cycles to the next
   deadline (UINT64_MAX when none). */
uint64_t obk_tick(struct obk_kernel *k, uint64_t now);

int obk_sema_create(struct obk_kernel *k, uint32_t attr, uint32_t option, int initial, int max);
int obk_sema_delete(struct obk_kernel *k, int id);
int obk_sema_signal(struct obk_kernel *k, int id);
int obk_sema_wait(struct obk_kernel *k, int id, int poll); /* OK, SEMA_ZERO or BLOCK */
int obk_sema_status(struct obk_kernel *k, int id, int *count, int *max, int *initial,
                    int *waiting, uint32_t *attr, uint32_t *option);

int obk_event_create(struct obk_kernel *k, uint32_t attr, uint32_t option, uint32_t bits);
int obk_event_delete(struct obk_kernel *k, int id);
int obk_event_set(struct obk_kernel *k, int id, uint32_t bits);
int obk_event_clear(struct obk_kernel *k, int id, uint32_t bits);
/* OK (result in *result), EVF_COND (poll), or BLOCK (result delivered later). */
int obk_event_wait(struct obk_kernel *k, int id, uint32_t bits, uint32_t mode, int poll,
                   uint32_t *result);
int obk_event_status(struct obk_kernel *k, int id, uint32_t *bits, uint32_t *initial,
                     int *waiting, uint32_t *attr, uint32_t *option);

/* Fixed pools. obk_fpl_bytes validates the parameters and returns the memory
   size the caller must supply (or an error); delete hands the memory back. */
int obk_fpl_bytes(uint32_t attr, int block_size, int blocks);
int obk_fpl_create(struct obk_kernel *k, uint32_t attr, uint32_t option, int block_size, int blocks,
                   void *mem);
int obk_fpl_delete(struct obk_kernel *k, int id, void **mem);
int obk_fpl_alloc(struct obk_kernel *k, int id, int poll, void **block); /* OK, NO_MEMORY, BLOCK */
int obk_fpl_free(struct obk_kernel *k, int id, void *block);
int obk_fpl_status(struct obk_kernel *k, int id, uint32_t *attr, uint32_t *option, int *block_size,
                   int *blocks, int *free_blocks, int *waiting);

/* Variable pools, backed by one heap chunk. */
int obk_vpl_bytes(uint32_t attr, int size);
int obk_vpl_create(struct obk_kernel *k, uint32_t attr, uint32_t option, int size, void *mem);
int obk_vpl_delete(struct obk_kernel *k, int id, void **mem);
int obk_vpl_alloc(struct obk_kernel *k, int id, int size, int poll, void **block);
int obk_vpl_free(struct obk_kernel *k, int id, void *block);
int obk_vpl_status(struct obk_kernel *k, int id, uint32_t *attr, uint32_t *option, int *size,
                   int *free_size, int *waiting);

/* Message boxes. */
int obk_mbx_create(struct obk_kernel *k, uint32_t attr, uint32_t option);
int obk_mbx_delete(struct obk_kernel *k, int id);
int obk_mbx_send(struct obk_kernel *k, int id, struct obk_message *msg);
int obk_mbx_receive(struct obk_kernel *k, int id, int poll, struct obk_message **msg); /* OK, NOMSG, BLOCK */
int obk_mbx_status(struct obk_kernel *k, int id, uint32_t *attr, uint32_t *option, int *waiting,
                   int *count, struct obk_message **top);

/* Heap chunks (no kernel state). init aligns mem to 8 and returns 0 when too small. */
#define OBK_CHUNK_OVERHEAD 24u /* Chunk header plus one block header. */
struct obk_chunk *obk_chunk_init(void *mem, uint32_t size);
struct obk_chunk *obk_chunk_at(void *mem);               /* Prepared chunk at mem, or 0. */
void *obk_chunk_alloc(struct obk_chunk *c, uint32_t size);
int obk_chunk_free(struct obk_chunk *c, void *block);    /* 0 or -1 */
int obk_chunk_contains(struct obk_chunk *c, void *p);
int obk_chunk_empty(struct obk_chunk *c);
uint32_t obk_chunk_free_size(struct obk_chunk *c);       /* Free payload bytes. */
uint32_t obk_chunk_largest(struct obk_chunk *c);         /* Largest single allocation. */

/* Growable heaps (heaplib) over a caller-supplied block allocator. Flag bit 0
   lets the heap grow, bit 1 takes memory from the top. */
struct obk_mem_ops { void *(*alloc)(uint32_t size, int high); void (*free)(void *p); };
#define OBK_HEAP_EXPAND 1
#define OBK_HEAP_HIGH 2
void *obk_heap_create(const struct obk_mem_ops *ops, int block_size, int flag);
void obk_heap_delete(const struct obk_mem_ops *ops, void *heap);
void *obk_heap_alloc(const struct obk_mem_ops *ops, void *heap, uint32_t size);
int obk_heap_free(const struct obk_mem_ops *ops, void *heap, void *p); /* 0, -1 bad block, -4 bad heap */
int obk_heap_free_size(void *heap);

/* Highest-priority ready thread (the running thread counts as ready). */
int obk_pick(struct obk_kernel *k);
/* Make id the running thread; a preempted runner keeps its queue position. */
void obk_switch(struct obk_kernel *k, int id);
struct obk_thread *obk_thread(struct obk_kernel *k, int id);
#endif
