/* SPDX-License-Identifier: MIT */
/* sifman/sifcmd libraries for IRX modules: IOP->EE DMA and commands, RPC server
   registration with per-thread request queues (public SifRpcServerData_t /
   SifRpcDataQueue_t layouts), and module command handlers. EE requests arrive
   through the native endpoint in sif.c/rpc.c and are routed here. */
#include "openbios/iop.h"
#include "openbios/sif.h"
#include "openbios/platform.h"

int ob_iop_register_builtin(struct ob_irx_exports *exports, uint32_t count);

typedef void *(*rpc_func)(int fno, void *buffer, int size);
struct server {
    int sid; rpc_func func; void *buf; int size;
    rpc_func cfunc; void *cbuf; int size2;
    uint32_t client, pkt_addr; int rpc_number;
    uint32_t recvbuf; int rsize, rmode, rid;
    struct server *link, *next;
    struct queue *base;
};
struct queue {
    int thread_id, active;
    struct server *link, *start, *end;
    struct queue *next;
};
struct dma_transfer { uint32_t src, dest; int size, attr; };
typedef int (*cmd_handler)(void *packet, void *harg);

static struct queue *queues;
struct cmd_entry { cmd_handler fn; void *arg; uint32_t gp; };
static struct cmd_entry user_handlers[32], system_handlers[32];
/* Tables a module installed with sceSifSetCmdBuffer/SetSysCmdBuffer (public
   SifCmdHandlerData_t {handler, arg} arrays); 0 selects the built-in ones. */
struct cmd_pair { cmd_handler fn; void *arg; };
#define TABLE_GP_MAX 64
static struct cmd_table { struct cmd_pair *pairs; int size; uint32_t gp[TABLE_GP_MAX]; } tables[2];
static uint32_t dma_id;

static uint32_t lock(void) {
    uint32_t v; __asm__ volatile ("mfc0 %0, $12\n\tnop" : "=r"(v));
    __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(v & ~1u) : "memory");
    return v;
}
static void unlock(uint32_t v) { __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(v) : "memory"); }
static uint32_t current_gp(void) { uint32_t g; __asm__ volatile ("move %0, $28" : "=r"(g)); return g; }

/* ---- routing of EE requests ---- */
static struct server *find_sid(int sid) {
    struct queue *q;
    struct server *s;
    for (q = queues; q; q = q->next)
        for (s = q->link; s; s = s->link)
            if (s->sid == sid) return s;
    return 0;
}
static struct server *find_server(uint32_t address) {
    struct queue *q;
    struct server *s;
    for (q = queues; q; q = q->next)
        for (s = q->link; s; s = s->link)
            if ((uint32_t)(uintptr_t)s == (address & 0x1fffffu)) return s;
    return 0;
}
int ob_iop_module_rpc(const struct ob_rpc_request *r) {
    struct server *s;
    uint32_t v;
    if (r->command == OB_SIF_CMD_RPC_BIND) {
        s = find_sid((int)r->sid);
        if (!s) return 0;
        ob_iop_rpc_reply(r, (uint32_t)(uintptr_t)s, (uint32_t)(uintptr_t)s->buf,
                         (uint32_t)(uintptr_t)s->cbuf, 0, 0);
        return 1;
    }
    if (r->command != OB_SIF_CMD_RPC_CALL) return 0;
    s = find_server(r->server);
    if (!s) return 0;
    v = lock();
    s->next = 0;
    if (s->base->start) s->base->end->next = s;
    else s->base->start = s;
    s->base->end = s;
    s->pkt_addr = r->pkt_addr;
    s->client = r->client;
    s->rpc_number = (int)r->rpc_number;
    s->size = (int)r->send_size;
    s->recvbuf = r->recvbuf;
    s->rsize = (int)r->recv_size;
    s->rmode = (int)r->rmode;
    s->rid = (int)r->rec_id;
    unlock(v);
    if (s->base->thread_id >= 0 && !s->base->active) ob_kernel_wakeup(s->base->thread_id);
    return 1;
}
int ob_iop_module_command(void *packet, uint32_t cid) {
    unsigned index = cid & 0x7fffffffu;
    struct cmd_table *t = &tables[cid >> 31];
    cmd_handler fn;
    void *arg;
    uint32_t saved, gp;
    if (t->pairs) {
        if ((int)index >= t->size || !t->pairs[index].fn) return 0;
        fn = t->pairs[index].fn;
        arg = t->pairs[index].arg;
        gp = index < TABLE_GP_MAX ? t->gp[index] : t->gp[0];
    } else {
        struct cmd_entry *h = (cid & 0x80000000u) ? &system_handlers[index & 0x1f] : &user_handlers[index & 0x1f];
        if (index >= 32 || !h->fn) return 0;
        fn = h->fn; arg = h->arg; gp = h->gp;
    }
    __asm__ volatile ("move %0, $28" : "=r"(saved));
    if (gp) __asm__ volatile ("move $28, %0" :: "r"(gp));
    fn(packet, arg);
    __asm__ volatile ("move $28, %0" :: "r"(saved));
    return 1;
}

/* ---- sifcmd: RPC server side ---- */
static void sceSifInitRpc(int mode) { (void)mode; }
static void sceSifSetRpcQueue(struct queue *q, int thread_id) {
    uint32_t v = lock();
    struct queue **at = &queues;
    q->thread_id = thread_id;
    q->active = 0;
    q->link = q->start = q->end = 0;
    q->next = 0;
    while (*at && *at != q) at = &(*at)->next;
    if (!*at) *at = q;
    unlock(v);
}
static void sceSifRegisterRpc(struct server *s, int sid, rpc_func func, void *buf, rpc_func cfunc,
                              void *cbuf, struct queue *q) {
    uint32_t v = lock();
    struct server **at = &q->link;
    s->sid = sid; s->func = func; s->buf = buf;
    s->cfunc = cfunc; s->cbuf = cbuf;
    s->next = s->link = 0;
    s->base = q;
    while (*at) at = &(*at)->link;
    *at = s;
    unlock(v);
}
static struct server *sceSifRemoveRpc(struct server *s, struct queue *q) {
    uint32_t v = lock();
    struct server **at = &q->link;
    int found;
    while (*at && *at != s) at = &(*at)->link;
    /* *at changes once unlinked */
    found = *at != 0;
    if (found) *at = s->link;
    unlock(v);
    return found ? s : 0;
}
static struct queue *sceSifRemoveRpcQueue(struct queue *q) {
    uint32_t v = lock();
    struct queue **at = &queues;
    while (*at && *at != q) at = &(*at)->next;
    if (*at) *at = q->next;
    unlock(v);
    return q;
}
static struct server *sceSifGetNextRequest(struct queue *q) {
    uint32_t v = lock();
    struct server *s = q->start;
    q->active = s != 0;
    q->start = s ? s->next : 0;
    unlock(v);
    return s;
}
static void exec_reply(struct server *s, void *result) {
    struct ob_rpc_request r;
    ob_zero(&r, sizeof(r));
    r.command = OB_SIF_CMD_RPC_CALL;
    r.rec_id = (uint32_t)s->rid;
    r.pkt_addr = s->pkt_addr;
    r.client = s->client;
    r.recvbuf = s->recvbuf;
    r.recv_size = (uint32_t)s->rsize;
    r.rmode = (uint32_t)s->rmode;
    ob_iop_rpc_reply(&r, (uint32_t)(uintptr_t)s, 0, 0, (uint32_t)(uintptr_t)result,
                     result && s->rsize > 0 ? (uint32_t)s->rsize : 0);
}
static void sceSifExecRequest(struct server *s) {
    uint32_t keep[32] __attribute__((aligned(16)));
    void *result = s->func(s->rpc_number, s->buf, s->size);
    /* Some servers return a buffer from their own (now dead) stack frame,
       e.g. Killer 7's sound module. Copy it up before any call reuses that
       stack. Plain volatile copy so it can't turn into a memcpy call. */
    if (result && (uintptr_t)result < (uintptr_t)keep &&
        (uintptr_t)keep - (uintptr_t)result <= 0x1000u &&
        s->rsize > 0 && (uint32_t)s->rsize <= sizeof(keep)) {
        volatile uint32_t *from = result, *to = keep;
        unsigned i;
        for (i = 0; i < ((uint32_t)s->rsize + 3) / 4; ++i) to[i] = from[i];
        result = keep;
    }
    exec_reply(s, result);
}
static void sceSifRpcLoop(struct queue *q) {
    for (;;) {
        struct server *s = sceSifGetNextRequest(q);
        if (s) sceSifExecRequest(s);
        else ob_kernel_sleep();
    }
}

/* ---- built-in RPC servers with their own threads ---- */
#define BUILTIN_SERVERS 8
static struct builtin_server {
    struct queue q;
    struct server s;
    int sid;
    rpc_func func;
    void *buf;
    struct server *current; /* request being served, until replied */
} builtin_servers[BUILTIN_SERVERS];
static void builtin_server_thread(void *arg) {
    struct builtin_server *b = arg;
    b->q.thread_id = ob_kernel_current_thread();
    for (;;) {
        struct server *s = sceSifGetNextRequest(&b->q);
        void *result;
        if (!s) { ob_kernel_sleep(); continue; }
        b->current = s;
        result = s->func(s->rpc_number, s->buf, s->size);
        if (b->current) exec_reply(s, result);
        b->current = 0;
    }
}
int ob_iop_rpc_reply_early(void *result) {
    int self = ob_kernel_current_thread();
    unsigned i;
    for (i = 0; i < BUILTIN_SERVERS; ++i) {
        struct builtin_server *b = &builtin_servers[i];
        if (b->func && b->current && b->q.thread_id == self) {
            struct server *s = b->current;
            b->current = 0;
            exec_reply(s, result);
            return 0;
        }
    }
    return -1;
}
int ob_iop_rpc_server_start(int sid, void *(*func)(int fno, void *buf, int size), void *buf,
                            int priority, uint32_t stack_size) {
    unsigned i;
    for (i = 0; i < BUILTIN_SERVERS; ++i) if (!builtin_servers[i].func) break;
    if (i == BUILTIN_SERVERS) return -1;
    builtin_servers[i].sid = sid;
    builtin_servers[i].func = func;
    builtin_servers[i].buf = buf;
    /* Register before the thread exists: the EE may bind as soon as the IOP
       reports ready; requests queued meanwhile wait for the loop. */
    sceSifSetRpcQueue(&builtin_servers[i].q, -1);
    sceSifRegisterRpc(&builtin_servers[i].s, sid, func, buf, 0, 0, &builtin_servers[i].q);
    return ob_kernel_spawn(builtin_server_thread, &builtin_servers[i], priority, stack_size);
}
/* Another server on the queue (and thread) of the built-in server `with`. */
int ob_iop_rpc_server_join(int sid, void *(*func)(int fno, void *buf, int size), void *buf, int with) {
    unsigned i, j;
    for (j = 0; j < BUILTIN_SERVERS; ++j) if (builtin_servers[j].func && builtin_servers[j].sid == with) break;
    for (i = 0; i < BUILTIN_SERVERS; ++i) if (!builtin_servers[i].func) break;
    if (i == BUILTIN_SERVERS || j == BUILTIN_SERVERS) return -1;
    builtin_servers[i].sid = sid;
    builtin_servers[i].func = func;
    builtin_servers[i].buf = buf;
    sceSifRegisterRpc(&builtin_servers[i].s, sid, func, buf, 0, 0, &builtin_servers[j].q);
    return 0;
}

/* ---- sifcmd: commands ---- */
static void sceSifInitCmd(void) {}
static uint32_t sceSifGetSreg(int index) { return ob_iop_sif_sreg((unsigned)index, 0, 0); }
static uint32_t sceSifSetSreg(int index, uint32_t value) {
    uint32_t old = ob_iop_sif_sreg((unsigned)index, 0, 0);
    ob_iop_sif_sreg((unsigned)index, 1, value);
    return old;
}
static void *set_cmd_table(int system, struct cmd_pair *pairs, int size) {
    struct cmd_table *t = &tables[system];
    void *old = t->pairs;
    unsigned i;
    t->pairs = pairs;
    t->size = size;
    for (i = 0; i < TABLE_GP_MAX; ++i) t->gp[i] = current_gp();
    return old;
}
static void *sceSifSetCmdBuffer(struct cmd_pair *pairs, int size) { return set_cmd_table(0, pairs, size); }
static void *sceSifSetSysCmdBuffer(struct cmd_pair *pairs, int size) { return set_cmd_table(1, pairs, size); }
static void sceSifAddCmdHandler(uint32_t cid, cmd_handler fn, void *arg) {
    unsigned index = cid & 0x1f;
    struct cmd_table *t = &tables[cid >> 31];
    if (t->pairs) {
        unsigned i = cid & 0x7fffffffu;
        if ((int)i < t->size) {
            t->pairs[i].fn = fn; t->pairs[i].arg = arg;
            if (i < TABLE_GP_MAX) t->gp[i] = current_gp();
        }
        return;
    }
    if (cid & 0x80000000u) {
        system_handlers[index].fn = fn; system_handlers[index].arg = arg;
        system_handlers[index].gp = current_gp();
    } else {
        user_handlers[index].fn = fn; user_handlers[index].arg = arg;
        user_handlers[index].gp = current_gp();
    }
}
static void sceSifRemoveCmdHandler(uint32_t cid) {
    unsigned index = cid & 0x1f;
    struct cmd_table *t = &tables[cid >> 31];
    if (t->pairs) {
        if ((int)(cid & 0x7fffffffu) < t->size) t->pairs[cid & 0x7fffffffu].fn = 0;
        return;
    }
    if (cid & 0x80000000u) system_handlers[index].fn = 0;
    else user_handlers[index].fn = 0;
}
/* Command header: psize (8 bits) | extra size (24 bits), extra destination, cid, opt. */
static uint32_t send_cmd(uint32_t cid, void *packet, int psize, void *src, void *dest, int size) {
    uint32_t *h = packet, buffer;
    struct ob_sif0_segment seg[2];
    unsigned n = 0;
    if (!packet || psize < 16 || psize > 112 || ob_iop_sif_ee_buffer(&buffer)) return 0;
    h[0] = (uint32_t)(psize & 0xff) | ((uint32_t)(size > 0 ? size : 0) << 8);
    h[1] = size > 0 ? (uint32_t)(uintptr_t)dest : 0;
    h[2] = cid;
    if (size > 0) {
        seg[n].iop_source = (uint32_t)(uintptr_t)src;
        seg[n].ee_destination = (uint32_t)(uintptr_t)dest;
        seg[n].qwc = ((uint32_t)size + 15) / 16;
        seg[n++].irq = 0;
    }
    seg[n].iop_source = (uint32_t)(uintptr_t)packet;
    seg[n].ee_destination = buffer;
    seg[n].qwc = ((uint32_t)psize + 15) / 16;
    seg[n++].irq = 1;
    if (ob_iop_sif0_send_chain(seg, n) != OB_OK) return 0;
    return ++dma_id ? dma_id : ++dma_id;
}
/* Internal: send a command packet, first transferring size bytes src->dest. */
uint32_t ob_iop_send_cmd(uint32_t cid, void *packet, int psize, void *src, uint32_t dest, int size) {
    return send_cmd(cid, packet, psize, src, (void *)(uintptr_t)dest, size);
}
static uint32_t sceSifSendCmd(uint32_t cid, void *p, int ps, void *s, void *d, int n) {
    return send_cmd(cid, p, ps, s, d, n);
}
static uint32_t sceSifSendCmdIntr(uint32_t cid, void *p, int ps, void *s, void *d, int n,
                                  void (*done)(void *), void *arg) {
    uint32_t id = send_cmd(cid, p, ps, s, d, n);
    if (id && done) done(arg); /* The transfer completes before returning. */
    return id;
}

/* ---- sifcmd: IOP as RPC client of the EE ---- */
/* Public SifRpcClientData_t / SifRpcReceiveData_t: header {pkt_addr, rpc_id,
   sema_id, mode}, then command, buf, cbuf, end_function, end_param, server
   (client) or src, dest, size (receive). */
struct client { uint32_t pkt_addr, rpc_id; int sema_id; uint32_t mode, command, buf, cbuf;
                void (*end_function)(void *); void *end_param; uint32_t server; };
#define WAITS 16
static struct { uint32_t cd; int thread, done, nowait; } waits[WAITS];
static uint32_t request_packets[WAITS][16] __attribute__((aligned(16)));
static uint32_t next_rpc_id;

static int wait_slot(uint32_t cd, int nowait) {
    uint32_t v = lock();
    int i;
    for (i = 0; i < WAITS; ++i) if (!waits[i].cd) {
        waits[i].cd = cd;
        waits[i].thread = ob_kernel_current_thread();
        waits[i].done = 0;
        waits[i].nowait = nowait;
        unlock(v);
        return i;
    }
    unlock(v);
    return -1;
}
static void wait_done(int slot) {
    /* Thread 1 delivers the EE's answers itself, so when it is the waiter it
       must keep polling SIF instead of sleeping. */
    while (!waits[slot].done) {
        if (ob_kernel_current_thread() == 1) { if (!ob_iop_sif_poll()) ob_kernel_pause(100); }
        else ob_kernel_sleep();
    }
    waits[slot].cd = 0;
}
/* RPC_END from the EE (public SifRpcRendPkt_t): cd at word 7, cid at word 8. */
int ob_iop_module_rpc_end(const uint32_t *packet) {
    uint32_t cd = packet[7], cid = packet[8];
    struct client *c = (struct client *)(uintptr_t)(cd & 0x1fffffu);
    int i;
    for (i = 0; i < WAITS; ++i) if (waits[i].cd && waits[i].cd == (cd & 0x1fffffu)) break;
    if (i == WAITS) return 0;
    if (cid == OB_SIF_CMD_RPC_BIND) { c->server = packet[9]; c->buf = packet[10]; c->cbuf = packet[11]; }
    c->pkt_addr = 0; /* sceSifCheckStatRpc: no longer busy */
#ifdef OB_TRACE_RPC
    ob_puts("[openbios:iopclient] end cid="); ob_puthex(cid); ob_puts(" nowait="); ob_puthex((uint32_t)waits[i].nowait);
    ob_puts(" cd="); ob_puthex(cd); ob_puts(" server="); ob_puthex(packet[9]); ob_puts("\n");
#endif
    if (cid == OB_SIF_CMD_RPC_CALL && c->end_function) c->end_function(c->end_param);
    if (waits[i].nowait) {
        waits[i].cd = 0;
        return 1;
    }
    waits[i].done = 1;
    ob_kernel_wakeup(waits[i].thread);
    return 1;
}
static int request(uint32_t cid, uint32_t cd, const uint32_t *words, unsigned count,
                   void *src, void *dest, int size, int nowait) {
    /* Every request holds a slot and packet until the EE answers, so no-wait
       requests still get their end callback and status. */
    int slot = wait_slot(cd & 0x1fffffu, nowait);
    uint32_t *p;
    unsigned i;
    if (slot < 0) return -1;
    p = request_packets[slot];
    ob_zero(p, 64);
    p[4] = 0;                                  /* rec_id */
    p[5] = (uint32_t)(uintptr_t)p;             /* pkt_addr */
    p[6] = ++next_rpc_id;                      /* rpc_id */
    for (i = 0; i < count; ++i) p[7 + i] = words[i];
    if (cid != 0x8000000cu) ((struct client *)(uintptr_t)cd)->pkt_addr = (uint32_t)(uintptr_t)p;
    if (!send_cmd(cid, p, 64, src, dest, size)) {
        if (cid != 0x8000000cu) ((struct client *)(uintptr_t)cd)->pkt_addr = 0;
        waits[slot].cd = 0;
        return -2;
    }
    if (!nowait) wait_done(slot);
    return 0;
}
static int sceSifBindRpc(struct client *c, int sid, int mode) {
    uint32_t w[2];
#ifdef OB_TRACE_RPC
    ob_puts("[openbios:iopclient] bind sid="); ob_puthex((uint32_t)sid); ob_puts("\n");
#endif
    int r, tries;
    c->command = 0; c->server = 0;
    w[0] = (uint32_t)(uintptr_t)c;
    w[1] = (uint32_t)sid;
    r = request(OB_SIF_CMD_RPC_BIND, (uint32_t)(uintptr_t)c, w, 2, 0, 0, 0, mode & 1);
    /* A blocking bind to an EE server that is not registered yet returns no
       server; retry briefly, since the EE may register it only after the
       binding module's start routine returns. */
    for (tries = 0; !(mode & 1) && !r && !c->server && tries < 500; ++tries) {
        ob_kernel_pause(2000);
        r = request(OB_SIF_CMD_RPC_BIND, (uint32_t)(uintptr_t)c, w, 2, 0, 0, 0, 0);
    }
    return r;
}
static int sceSifCallRpc(struct client *c, int fno, int mode, void *send, int ssize, void *recv,
                         int rsize, void (*end)(void *), void *end_param) {
    uint32_t w[7];
    c->end_function = end;
    c->end_param = end_param;
    w[0] = (uint32_t)(uintptr_t)c;
    w[1] = (uint32_t)fno;
    w[2] = (uint32_t)ssize;
    w[3] = (uint32_t)(uintptr_t)recv;
    w[4] = (uint32_t)rsize;
    w[5] = 1; /* rmode: the EE answers with an RPC_END command. */
    w[6] = c->server;
#ifdef OB_TRACE_RPC
    ob_puts("[openbios:iopclient] call fno="); ob_puthex((uint32_t)fno); ob_puts(" mode="); ob_puthex((uint32_t)mode);
    ob_puts(" ssize="); ob_puthex((uint32_t)ssize); ob_puts(" recv="); ob_puthex((uint32_t)(uintptr_t)recv);
    ob_puts(" rsize="); ob_puthex((uint32_t)rsize); ob_puts(" server="); ob_puthex(c->server); ob_puts("\n");
#endif
#ifdef OB_TRACE_RPC
    {
        int r = request(OB_SIF_CMD_RPC_CALL, (uint32_t)(uintptr_t)c, w, 7, send,
                        (void *)(uintptr_t)c->buf, ssize > 0 ? ssize : 0, mode & 1);
        unsigned k;
        ob_puts("[openbios:iopclient] send");
        for (k = 0; k < 4 && send && (int)(4 * k) < ssize; ++k) { ob_puts(" "); ob_puthex(((uint32_t *)send)[k]); }
        ob_puts(" recv");
        for (k = 0; k < 4 && recv && (int)(4 * k) < rsize; ++k) { ob_puts(" "); ob_puthex(((uint32_t *)recv)[k]); }
        ob_puts("\n");
        return r;
    }
#endif
    return request(OB_SIF_CMD_RPC_CALL, (uint32_t)(uintptr_t)c, w, 7, send,
                   (void *)(uintptr_t)c->buf, ssize > 0 ? ssize : 0, mode & 1);
}
static int sceSifCheckStatRpc(struct client *c) { return c->pkt_addr != 0; }
/* Ask the EE to DMA size bytes from its src into our dest. */
static int sceSifGetOtherData(struct client *rd, void *src, void *dest, int size, int mode) {
    uint32_t w[4];
    w[0] = (uint32_t)(uintptr_t)rd;
    w[1] = (uint32_t)(uintptr_t)src;
    w[2] = (uint32_t)(uintptr_t)dest;
    w[3] = (uint32_t)size;
    return request(0x8000000cu, (uint32_t)(uintptr_t)rd, w, 4, 0, 0, 0, mode & 1);
}
/* Internal: copy size bytes of EE memory at ee_src into IOP memory (waits). */
int ob_iop_get_other_data(uint32_t ee_src, void *dest, int size) {
    struct client rd;
    ob_zero(&rd, sizeof(rd));
    return sceSifGetOtherData(&rd, (void *)(uintptr_t)ee_src, dest, size, 0);
}

/* ---- sifman ---- */
#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
static void sceSifInit(void) {}
static int sceSifCheckInit(void) { return 1; }
/* IOP->EE transfers: INT_O (0x4) raises the EE interrupt for that transfer. */
static uint32_t sceSifSetDma(struct dma_transfer *t, int count) {
    struct ob_sif0_segment seg[4];
    int i, n = 0;
    for (i = 0; i < count; ++i) {
        if (t[i].size <= 0) continue;
        seg[n].iop_source = t[i].src;
        seg[n].ee_destination = t[i].dest;
        seg[n].qwc = ((uint32_t)t[i].size + 15) / 16;
        seg[n++].irq = (t[i].attr & 4) != 0;
        if (n == 4 || i + 1 == count) {
            if (ob_iop_sif0_send_chain(seg, (unsigned)n) != OB_OK) goto rejected;
            n = 0;
        }
    }
    if (n && ob_iop_sif0_send_chain(seg, (unsigned)n) != OB_OK) goto rejected;
    return ++dma_id ? dma_id : ++dma_id;
rejected:
    ob_puts("[openbios:iop] sceSifSetDma rejected src="); ob_puthex(t[0].src); ob_puts(" dest=");
    ob_puthex(t[0].dest); ob_puts(" size="); ob_puthex((uint32_t)t[0].size); ob_puts(" count=");
    ob_puthex((uint32_t)count); ob_puts("\n");
    return 0;
}
static int sceSifDmaStat(uint32_t id) { (void)id; return -1; } /* Sends complete synchronously. */
static uint32_t sceSifSetDmaIntr(struct dma_transfer *t, int count, void (*done)(void *), void *arg) {
    uint32_t id = sceSifSetDma(t, count);
    if (id && done) done(arg);
    return id;
}
static uint32_t sceSifGetMSFlag(void) { return REG32(0xbd000020u); }
static uint32_t sceSifSetMSFlag(uint32_t v) { REG32(0xbd000020u) = v; return REG32(0xbd000020u); }
static uint32_t sceSifGetSMFlag(void) { return REG32(0xbd000030u); }
static uint32_t sceSifSetSMFlag(uint32_t v) { REG32(0xbd000030u) = v; return REG32(0xbd000030u); }
static uint32_t sceSifGetMainAddr(void) { return REG32(0xbd000000u); }
static uint32_t sceSifGetSubAddr(void) { return REG32(0xbd000010u); }
static uint32_t sceSifSetSubAddr(uint32_t v) { REG32(0xbd000010u) = v; return v; }
static int sceSifIsSending(void) { return (REG32(0xbf801528u) & 0x01000000u) != 0; }

static int ret_zero(void) { return 0; }
#define LIBRARY(var, text, size) \
    static struct { uint32_t magic, next; uint16_t version, mode; \
                    char name[8] __attribute__((nonstring)); uint32_t f[size + 1]; } var = { \
        OB_IRX_EXPORT_MAGIC, 0, 0x0101, 0, text, {0}}
#define FN(p) ((uint32_t)(uintptr_t)(p))
LIBRARY(sifman, "sifman", 36);
LIBRARY(sifcmd, "sifcmd", 32);

void ob_iop_sifrpc_init(void) {
    unsigned i;
    queues = 0;
    ob_zero(builtin_servers, sizeof(builtin_servers)); /* their threads died with the reboot */
    ob_zero(waits, sizeof(waits));
    ob_zero(user_handlers, sizeof(user_handlers));
    ob_zero(tables, sizeof(tables));
    ob_zero(system_handlers, sizeof(system_handlers));
    for (i = 0; i < 4; ++i) sifman.f[i] = sifcmd.f[i] = FN(ret_zero);
    sifman.f[5] = FN(sceSifInit);
    sifman.f[6] = FN(ret_zero); /* sceSifSetDChain: the native endpoint keeps SIF1 armed. */
    sifman.f[7] = FN(sceSifSetDma);
    sifman.f[8] = FN(sceSifDmaStat);
    sifman.f[11] = FN(sceSifIsSending);
    sifman.f[21] = FN(sceSifGetMSFlag); sifman.f[22] = FN(sceSifSetMSFlag);
    sifman.f[23] = FN(sceSifGetSMFlag); sifman.f[24] = FN(sceSifSetSMFlag);
    sifman.f[25] = FN(sceSifGetMainAddr); sifman.f[26] = FN(sceSifGetSubAddr);
    sifman.f[27] = FN(sceSifSetSubAddr);
    sifman.f[29] = FN(sceSifCheckInit);
    sifman.f[32] = FN(sceSifSetDmaIntr);
    sifcmd.f[4] = FN(sceSifInitCmd);
    sifcmd.f[6] = FN(sceSifGetSreg); sifcmd.f[7] = FN(sceSifSetSreg);
    sifcmd.f[8] = FN(sceSifSetCmdBuffer); sifcmd.f[9] = FN(sceSifSetSysCmdBuffer);
    sifcmd.f[10] = FN(sceSifAddCmdHandler); sifcmd.f[11] = FN(sceSifRemoveCmdHandler);
    sifcmd.f[12] = FN(sceSifSendCmd); sifcmd.f[13] = FN(sceSifSendCmd);
    sifcmd.f[14] = FN(sceSifInitRpc);
    sifcmd.f[15] = FN(sceSifBindRpc);
    sifcmd.f[16] = FN(sceSifCallRpc);
    sifcmd.f[17] = FN(sceSifRegisterRpc);
    sifcmd.f[18] = FN(sceSifCheckStatRpc);
    sifcmd.f[23] = FN(sceSifGetOtherData);
    sifcmd.f[19] = FN(sceSifSetRpcQueue);
    sifcmd.f[20] = FN(sceSifGetNextRequest);
    sifcmd.f[21] = FN(sceSifExecRequest);
    sifcmd.f[22] = FN(sceSifRpcLoop);
    sifcmd.f[24] = FN(sceSifRemoveRpc);
    sifcmd.f[25] = FN(sceSifRemoveRpcQueue);
    sifcmd.f[28] = FN(sceSifSendCmdIntr); sifcmd.f[29] = FN(sceSifSendCmdIntr);
    ob_iop_register_builtin((void *)&sifman, 36);
    ob_iop_register_builtin((void *)&sifcmd, 32);
}
