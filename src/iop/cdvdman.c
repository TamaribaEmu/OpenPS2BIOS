/* SPDX-License-Identifier: MIT */
/* cdvdman (public interface): drive commands, asynchronous sector reads with
   sceCdSync/sceCdCallback, ISO9660 file lookup and MECHACON S-commands. Reads
   are served by a worker thread; only 2048-byte user sectors are supported. */
#include "openbios/iop.h"
#include "openbios/disc.h"
#include "openbios/iso9660.h"
#include "openbios/ioman.h"
#include "openbios/platform.h"

#define CDVD_REG(n) (*(volatile uint8_t *)(uintptr_t)(0xbf402000u + (n)))
int ob_iop_register_builtin(struct ob_irx_exports *exports, uint32_t count);

static uint32_t irq_lock(void) {
    uint32_t s; __asm__ volatile ("mfc0 %0, $12\n\tnop" : "=r"(s));
    __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s & ~1u) : "memory");
    return s;
}
static void irq_unlock(uint32_t s) { __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s) : "memory"); }

/* Issue one S-command; up to 16 result bytes. Interrupts stay off so threads
   cannot interleave commands on the shared registers. Returns bytes read or -1. */
int ob_cd_scommand(uint8_t cmd, const void *params, unsigned count, void *result, unsigned want) {
    const uint8_t *in = params;
    uint8_t *out = result;
    unsigned i, n = 0;
    uint32_t s = irq_lock();
    for (i = 0; i < 2000000 && (CDVD_REG(0x17) & 0x80); ++i) {}
    if (CDVD_REG(0x17) & 0x80) { irq_unlock(s); return -1; }
    while (!(CDVD_REG(0x17) & 0x40)) (void)CDVD_REG(0x18); /* Drain stale results. */
    for (i = 0; i < count; ++i) CDVD_REG(0x17) = in[i];
    CDVD_REG(0x16) = cmd;
    for (i = 0; i < 2000000 && (CDVD_REG(0x17) & 0x80); ++i) {}
    if (CDVD_REG(0x17) & 0x80) { irq_unlock(s); return -1; }
    while (!(CDVD_REG(0x17) & 0x40) && n < 16) {
        uint8_t v = CDVD_REG(0x18);
        if (out && n < want) out[n] = v;
        ++n;
    }
    irq_unlock(s);
    return (int)n;
}
/* Returns 1 on success and always fills 16 bytes of output. */
static int sceCdApplySCmd(uint8_t cmd, const void *in, uint16_t in_size, void *out) {
    uint8_t buffer[16];
    int n;
    unsigned i;
    for (i = 0; i < 16; ++i) buffer[i] = 0;
    n = ob_cd_scommand(cmd, in, in_size, buffer, 16);
    if (out) for (i = 0; i < 16; ++i) ((uint8_t *)out)[i] = buffer[i];
    return n < 0 ? 0 : 1;
}
/* sceCdCLOCK: status, second, minute, hour, pad, day, month, year (BCD). */
static int sceCdReadClock(void *clock) {
    uint8_t r[8] = {0};
    int n = ob_cd_scommand(0x08, 0, 0, r, 8);
    unsigned i;
    for (i = 0; i < 8; ++i) ((uint8_t *)clock)[i] = r[i];
    return n >= 8 && !(r[0] & 0x80);
}
static int sceCdStatus(void) { return CDVD_REG(0x0a); }
static int sceCdGetDiskType(void) { return CDVD_REG(0x0f); }

/* Public error codes and callback function codes. */
#define ERR_NONE 0x00
#define ERR_ABORT 0x01
#define ERR_PARAM 0x22
#define ERR_READ 0x30
#define FUNC_READ 1
#define FUNC_SEEK 4
#define FUNC_STANDBY 5
#define FUNC_STOP 6
#define FUNC_PAUSE 7
#define FUNC_BREAK 8

struct read_mode { uint8_t trycount, spindlctrl, datapattern, pad; };
static volatile int last_error;
static volatile int busy, abort_read, worker = -1;
static volatile uint32_t read_lba, read_count, read_done;
static uint8_t *read_buffer;
static uint32_t callback, callback_gp;
/* Stream mode (sceCdSC 0-2): reads completed in mode 1 or 2 also go to the
   callback the stream driver registered with sceCdstm0Cb/sceCdstm1Cb. */
static volatile int stream_flag;
static uint32_t stream_cb[2], stream_gp[2];


static uint32_t current_gp(void) { uint32_t g; __asm__ volatile ("move %0, $28" : "=r"(g)); return g; }
/* The user callback runs with its module's gp. */
static void call_with_gp(uint32_t fn, uint32_t gp, int value) {
    uint32_t saved;
    if (!fn) return;
    __asm__ volatile ("move %0, $28" : "=r"(saved));
    __asm__ volatile ("move $28, %0" :: "r"(gp));
    ((void (*)(int))(uintptr_t)fn)(value);
    __asm__ volatile ("move $28, %0" :: "r"(saved));
}
static void notify(int function) { call_with_gp(callback, callback_gp, function); }
/* Unaligned destinations go through a bounce buffer, one chunk at a time. */
#define BOUNCE_SECTORS 8
static uint8_t *bounce;
static int copy_out(void *ctx, const uint8_t *data, uint32_t first, uint32_t n) {
    (void)ctx;
    ob_copy(read_buffer + first * 2048u, data, n * 2048u);
    read_done = first + n;
    return abort_read;
}
#ifdef OB_TRACE_RPC
static void trace(const char *what, uint32_t a, uint32_t b, uint32_t c) {
    ob_puts("[openbios:cdvdman] "); ob_puts(what); ob_puts(" "); ob_puthex(a); ob_puts(" ");
    ob_puthex(b); ob_puts(" "); ob_puthex(c); ob_puts("\n");
}
#else
#define trace(what, a, b, c) ((void)0)
#endif
static void read_worker(void *arg) {
    volatile uint32_t detail;
    (void)arg;
    for (;;) {
        int r;
        while (!busy) ob_kernel_sleep();
        if (abort_read) r = OB_EFORMAT;
        else if (!((uintptr_t)read_buffer & 3)) {
            uint8_t *const direct[2] = {read_buffer, read_buffer};
            r = ob_iop_cd_read_stream(read_lba, read_count, direct, read_count, 0, 0, &detail);
            if (!r) read_done = read_count;
        } else {
            uint8_t *const halves[2] = {bounce, bounce + BOUNCE_SECTORS * 2048u};
            r = ob_iop_cd_read_stream(read_lba, read_count, halves, BOUNCE_SECTORS, copy_out, 0,
                                      &detail);
        }
        last_error = r == OB_OK ? ERR_NONE : abort_read ? ERR_ABORT : ERR_READ;
        busy = 0;
        trace("done", (uint32_t)r, (uint32_t)last_error, read_count);
        if (stream_flag == 1 || stream_flag == 2)
            call_with_gp(stream_cb[stream_flag - 1], stream_gp[stream_flag - 1], 1);
        notify(FUNC_READ);
    }
}
/* CreateThread fails in interrupt context, where some sound drivers issue
   their reads, so sceCdInit starts the worker ahead of time. */
static int start_worker(void) {
    if (worker <= 0) worker = ob_kernel_spawn(read_worker, 0, 10, 0x1000); /* also runs the user callback */
    return worker > 0;
}
static int sceCdRead(uint32_t lba, uint32_t count, void *buffer, const struct read_mode *mode) {
    trace("read", lba, count, (uint32_t)(uintptr_t)buffer);
    trace("read mode/busy", mode ? *(const uint32_t *)(const void *)mode : 0xffffffffu, (uint32_t)busy, 0);
    if (busy) return 0;
    if (!count || (mode && mode->datapattern != 0)) { last_error = ERR_PARAM; return 0; }
    if (((uintptr_t)buffer & 3) && !bounce) {
        bounce = ob_sysmem_alloc(OB_ALLOC_LAST, 2 * BOUNCE_SECTORS * 2048u, 0);
        if (!bounce) return 0;
    }
    if (!start_worker()) return 0;
    read_lba = lba;
    read_count = count;
    read_done = 0;
    read_buffer = buffer;
    abort_read = 0;
    last_error = ERR_NONE;
    busy = 1;
    ob_kernel_wakeup(worker);
    return 1;
}
/* mode 0 (and other even modes): wait; odd modes: poll. 1 while busy. */
static int sceCdSync(int mode) {
    static int traced;
    if (traced++ < 50) trace("sync", (uint32_t)mode, (uint32_t)busy, 0);
    if (mode & 1) return busy ? 1 : 0;
    while (busy) if (!ob_kernel_pause(200)) {}
    return 0;
}
static int drive_ready(void) { return !busy && (CDVD_REG(0x05) & 0x40); }
/* 2 = complete (ready), 6 = not ready. */
static int sceCdDiskReady(int mode) {
    if (mode == 0) while (!drive_ready()) if (!ob_kernel_pause(200)) {}
    return drive_ready() ? 2 : 6;
}
static int sceCdCheckCmd(void) { return !busy; }
static int sceCdGetError(void) { return last_error; }
/* Bytes of the current or last read transferred so far; the count stays at the
   full size after the read completes. Direct reads are tracked through the
   drive DMA's address register. */
static uint32_t sceCdGetReadPos(void) {
    uint32_t total = read_count * 2048u, pos;
    if (!busy || ((uintptr_t)read_buffer & 3)) return read_done * 2048u;
    pos = (*(volatile uint32_t *)0xbf8010b0u & 0x1fffffu) - ((uint32_t)(uintptr_t)read_buffer & 0x1fffffu);
    return pos <= total ? pos : 0;
}
static int sceCdBreak(void) {
    if (!busy) return 0;
    abort_read = 1;
    notify(FUNC_BREAK);
    return 1;
}
static uint32_t sceCdCallback(uint32_t fn) {
    uint32_t old = callback;
    callback = fn;
    callback_gp = current_gp();
    return old;
}
/* Drive commands without data; the callback reports completion. */
static int simple_command(uint8_t cmd, const uint8_t *params, unsigned count, int function) {
    int r;
    if (busy) return 0;
    r = ob_iop_cd_ncommand(cmd, params, count);
    last_error = r < 0 ? ERR_READ : r;
    if (function) notify(function);
    return r == 0;
}
static int sceCdNop(void) { return simple_command(0, 0, 0, 0); }
static int sceCdStandby(void) { return simple_command(2, 0, 0, FUNC_STANDBY); }
static int sceCdStop(void) { return simple_command(3, 0, 0, FUNC_STOP); }
static int sceCdPause(void) { return simple_command(4, 0, 0, FUNC_PAUSE); }
static int sceCdSeek(uint32_t lba) {
    uint8_t p[4];
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (uint8_t)(lba >> (8 * i));
    return simple_command(5, p, 4, FUNC_SEEK);
}
static int sceCdInit(int mode) {
    start_worker();
    if (mode == 5) { /* exit: stop reading and drop the callback */
        if (busy) abort_read = 1;
        sceCdSync(0);
        callback = 0;
    }
    return 1;
}
/* The media type selects read modes; the drive reports it, so nothing to store. */
static int sceCdMmode(int media) { (void)media; return 1; }
/* sceCdlLOCCD: minute, second, sector, track (BCD), 150-sector lead-in. */
static int from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int sceCdPosToInt(const uint8_t *loc) {
    return (from_bcd(loc[0]) * 60 + from_bcd(loc[1])) * 75 + from_bcd(loc[2]) - 150;
}
static uint8_t *sceCdIntToPos(int lsn, uint8_t *loc) {
    lsn += 150;
    loc[0] = to_bcd(lsn / (60 * 75));
    loc[1] = to_bcd((lsn / 75) % 60);
    loc[2] = to_bcd(lsn % 75);
    loc[3] = 0;
    return loc;
}
/* sceCdlFILE: lsn, size, name[16], date[8] (pad, s, m, h, day, month, year16). */
struct cd_file { uint32_t lsn, size; char name[16]; uint8_t date[8]; };
/* layer 0: the disc's (first) volume; 1: a dual-layer DVD's second volume,
   whose LSNs are reported from the start of the disc. */
static int search_file(struct cd_file *fp, const char *path, int layer) {
    struct ob_iso_entry e;
    uint32_t offset = 0;
    const struct ob_iso *iso = layer ? ob_iop_cd_iso_layer1(&offset) : ob_iop_cd_iso();
    const char *base = path;
    unsigned i, year;
#ifdef OB_TRACE_RPC
    ob_puts("[openbios:cdvdman] search "); ob_puts(path); ob_puts("\n");
#endif
    /* Directories count too, the root doesn't. */
    if (!iso || ob_iso_lookup(iso, path, &e) || (e.flags & 2 && e.name[0] == '.')) return 0;
    for (i = 0; path[i]; ++i) if (path[i] == '\\' || path[i] == '/' || path[i] == ':') base = path + i + 1;
    ob_zero(fp, sizeof(*fp));
    fp->lsn = e.lba + offset;
    fp->size = e.size;
    for (i = 0; i < 15 && base[i]; ++i) fp->name[i] = base[i];
    year = 1900u + e.date[0];
    fp->date[1] = e.date[5];
    fp->date[2] = e.date[4];
    fp->date[3] = e.date[3];
    fp->date[4] = e.date[2];
    fp->date[5] = e.date[1];
    fp->date[6] = (uint8_t)year;
    fp->date[7] = (uint8_t)(year >> 8);
    return 1;
}
/* Lookups need far more stack than many module threads have, so searches run
   one at a time on a dedicated stack. */
int ob_iop_call_on_stack(int (*fn)(void *), void *arg, void *stack_top);
static uint8_t search_stack[12 * 1024] __attribute__((aligned(16)));
static volatile int search_busy;
/* Also used by the cdrom device in ioman (open/dopen/dread/getstat all walk
   ISO sectors). fn must not call back in here, it would wait on itself. */
int ob_iop_cd_on_stack(int (*fn)(void *), void *arg) {
    int r;
    for (;;) {
        uint32_t s = irq_lock();
        if (!search_busy) { search_busy = 1; irq_unlock(s); break; }
        irq_unlock(s);
        ob_kernel_pause(200);
    }
    r = ob_iop_call_on_stack(fn, arg, search_stack + sizeof(search_stack));
    search_busy = 0;
    return r;
}
struct search_args { struct cd_file *fp; const char *path; int layer; };
static int search_thunk(void *p) {
    struct search_args *a = p;
    return search_file(a->fp, a->path, a->layer);
}
static int search_on_stack(struct cd_file *fp, const char *path, int layer) {
    struct search_args a = {fp, path, layer};
    return ob_iop_cd_on_stack(search_thunk, &a);
}
static int sceCdSearchFile(struct cd_file *fp, const char *path) { return search_on_stack(fp, path, 0); }
static int sceCdLayerSearchFile(struct cd_file *fp, const char *path, int layer) {
    return layer == 0 || layer == 1 ? search_on_stack(fp, path, layer) : 0;
}
/* sceCdRI: the drive's 8-byte i.LINK ID (S-command 0x12). */
static int sceCdRI(uint8_t *id, uint32_t *result) {
    uint8_t r[9] = {0};
    int n = ob_cd_scommand(0x12, 0, 0, r, 9);
    unsigned i;
    if (n < 9) return 0;
    if (result) *result = r[0];
    for (i = 0; i < 8; ++i) id[i] = r[1 + i];
    return 1;
}
/* From the DNAS version of cdvdman. */
static int sceCdReadGUID(uint32_t *guid) {
    uint8_t id[8];
    uint32_t result = 1;
    if (!sceCdRI(id, &result) || result) return 0;
    guid[0] = id[0] | 0x08004600u;
    guid[1] = id[4] | id[5] << 8 | id[6] << 16 | (uint32_t)id[7] << 24;
    return 1;
}
static int sceCdReadModelID(uint32_t *model) {
    uint8_t id[8];
    uint32_t result = 1;
    if (!sceCdRI(id, &result) || result) return 0;
    *model = id[1] | id[2] << 8 | id[3] << 16;
    return 1;
}
/* For the EE S-command RPC: GUID (2 words) or model ID (1 word). */
int ob_iop_cd_read_id(int model, uint32_t *out) {
    return model ? sceCdReadModelID(out) : sceCdReadGUID(out);
}
static int sceCdReadDiskID(uint8_t *id) {
    static uint8_t sector[2048];
    static const uint8_t params[7] = {0, 0, 0, 0x4b, 0, 0, 0};
    uint8_t type = CDVD_REG(0x0f);
    unsigned i;
    for (i = 0; i < 5; ++i) id[i] = 0;
    if (type < 0x12 || type > 0x14) return 0;
    if (busy || ob_iop_cd_read_sectors(0x4b, 1, sector)) return 0;
    if (ob_iop_cd_ncommand(0x0c, params, sizeof(params))) return 0; /* read key */
    if (!(CDVD_REG(0x38) & 4)) return 0;
    for (i = 0; i < 5; ++i) id[i] = CDVD_REG(0x30 + i) ^ CDVD_REG(0x39);
    return 1;
}
/* sceCdRC: the raw MECHACON clock read (S-command 0x08, 8 bytes). */
static int sceCdRC(void *clock) {
    uint8_t r[8] = {0};
    int n = ob_cd_scommand(0x08, 0, 0, r, 8);
    ob_copy(clock, r, 8);
    return n >= 8;
}
/* sceCdRM: the 16-byte model name, read in two halves (S-command 0x17). */
static int sceCdRM(char *name, uint32_t *status) {
    uint8_t p, r[9];
    unsigned half;
    if (status) *status = 0;
    for (half = 0; half < 2; ++half) {
        p = (uint8_t)(half * 8);
        ob_zero(r, sizeof(r));
        if (ob_cd_scommand(0x17, &p, 1, r, 9) < 9) return 0;
        if (status) *status |= r[0];
        ob_copy(name + half * 8, r + 1, 8);
    }
    return 1;
}
/* sceCdTrayReq: 0 open, 1 close (S-command 0x06), 2 report whether the disc
   changed since the previous check. The first check after the driver starts
   reports a change, like a console's: Monster House's streamer only sets up
   (and lets its module start return) once it has seen one. */
static int last_disk_type = -1, first_check;
int ob_iop_cd_tray_check(void) {
    int type = CDVD_REG(0x0f), changed = first_check || type != last_disk_type;
    first_check = 0;
    last_disk_type = type;
    return changed;
}
static int sceCdTrayReq(int mode, uint32_t *changed) {
    if (mode == 2) {
        int c = ob_iop_cd_tray_check();
        if (changed) *changed = (uint32_t)c;
        return 1;
    }
    if (mode == 0 || mode == 1) {
        uint8_t p = (uint8_t)mode, r[1] = {0};
        return ob_cd_scommand(0x06, &p, 1, r, 1) >= 1 && !r[0];
    }
    return 0;
}
/* CD-DA needs raw 2352-byte transfers, which this firmware does not issue. */
static int sceCdReadCDDA(void) { last_error = ERR_PARAM; return 0; }
static int sceCdGetToc(uint8_t *toc) {
    int r;
    if (busy) return 0;
    r = ob_iop_cd_read_toc(toc);
    last_error = r < 0 ? ERR_READ : ERR_NONE;
    return r > 0;
}
static int sceCdReadDvdDualInfo(int *on_dual, uint32_t *layer1_start) {
    return ob_iop_cd_layer_info(on_dual, layer1_start);
}
static int sceCdstm0Cb(uint32_t fn) { stream_cb[0] = fn; stream_gp[0] = current_gp(); return 0; }
static int sceCdstm1Cb(uint32_t fn) { stream_cb[1] = fn; stream_gp[1] = current_gp(); return 0; }
/* Power-off: the registered handler, and S-command 0x0F to the MECHACON. */
static uint32_t poff_fn, poff_arg;
static uint32_t sceCdPOffCallback(uint32_t fn, uint32_t userdata) {
    uint32_t old = poff_fn;
    poff_fn = fn;
    poff_arg = userdata;
    return old;
}
static int sceCdPowerOff(uint32_t *status) {
    uint8_t r[1] = {0};
    int n = ob_cd_scommand(0x0f, 0, 0, r, 1);
    if (status) *status = r[0];
    return n >= 1;
}
static int sync_timeout = 15000, stream_timeout = 5000;
/* sceCdSC: -1 reads, 0-2 set the stream mode (both report the last error
   through *param); -2 sets the last error. */
static int sceCdSC(int code, int *param) {
    trace("sc", (uint32_t)code, 0, 0);
    switch (code) {
    case -1: case 0: case 1: case 2:
        *param = (uint8_t)last_error;
        if (code != -1) stream_flag = code;
        return stream_flag;
    case -2:
        last_error = *(uint8_t *)param;
        return (uint8_t)last_error;
    case -26: /* power-off handler and its argument */
        *param = (int)poff_fn;
        return (int)poff_arg;
    case -17: return stream_timeout;
    case -9: return 0x0101; /* module version */
    case -15: return sync_timeout;
    default:
        return 0;
    }
}
/* sceCdRE: a read that does not take the filesystem's read slot. */
static int sceCdRE(uint32_t lba, uint32_t count, void *buffer, const struct read_mode *mode) {
    return sceCdRead(lba, count, buffer, mode);
}
/* The stream API is served by the stream driver's "cdrom_stm0:" device. */
struct stm_request { uint32_t arg1, arg2; void *buffer; uint32_t cmd; struct read_mode mode; uint32_t error; };
/* Without a stream driver (on a console it is CDVDSTM, from ROM), the stream
   is served here like the EE stream RPC: sectors are read on demand at the
   stream position, so every read finds them "buffered" and STAT reports a
   full buffer. Gauntlet Dark Legacy's sound driver streams this way. */
static struct { int ready, active; uint32_t lba, buffer_sectors; } stm;
static int stm_read(uint32_t sectors, uint8_t *buffer, uint32_t *error) {
    uint32_t done = 0;
    int r = 0;
    if (!stm.active || !sectors) { if (error) *error = stm.active ? ERR_NONE : ERR_READ; return 0; }
    if (!((uintptr_t)buffer & 3)) r = ob_iop_cd_read_sectors(stm.lba, sectors, buffer);
    else {
        if (!bounce && !(bounce = ob_sysmem_alloc(OB_ALLOC_LAST, 2 * BOUNCE_SECTORS * 2048u, 0))) r = -1;
        while (!r && done < sectors) {
            uint32_t n = sectors - done < BOUNCE_SECTORS ? sectors - done : BOUNCE_SECTORS;
            r = ob_iop_cd_read_sectors(stm.lba + done, n, bounce);
            if (!r) ob_copy(buffer + done * 2048u, bounce, n * 2048u);
            done += n;
        }
    }
    if (error) *error = r ? ERR_READ : ERR_NONE;
    if (r) return 0;
    stm.lba += sectors;
    return (int)sectors;
}
static int stm_native(uint32_t cmd, uint32_t arg1, uint32_t arg2, void *buffer, int code, uint32_t *error) {
    if (code == 0x4394) return stm_read(arg2, buffer, error);
    switch (cmd) {
    case 5: stm.ready = 1; stm.active = 0; stm.buffer_sectors = arg1; return 1; /* INIT */
    case 1: if (!stm.ready) return 0; stm.active = 1; stm.lba = arg1; return 1;  /* START */
    case 4: case 9: stm.lba = arg1; return 1;                                    /* SEEK, SEEKF */
    case 3: stm.active = 0; return 1;                                            /* STOP */
    case 6: return stm.active ? (int)stm.buffer_sectors : 0;                     /* STAT */
    case 7: case 8: return 1;                                                    /* PAUSE, RESUME */
    default: return 0;
    }
}
static int stm_call(uint32_t cmd, uint32_t arg1, uint32_t arg2, void *buffer, const struct read_mode *mode,
                    int code, uint32_t *error) {
    struct stm_request q;
    int out = 0;
    trace("stm", cmd, arg1, arg2);
    ob_zero(&q, sizeof(q));
    q.cmd = cmd; q.arg1 = arg1; q.arg2 = arg2; q.buffer = buffer;
    if (mode) q.mode = *mode;
    if (ob_io_devctl("cdrom_stm0:", code, &q, sizeof(q), &out, 4) < 0)
        return stm_native(cmd, arg1, arg2, buffer, code, error);
    if (error) *error = q.error;
    return out;
}
static int sceCdStInit(uint32_t bufmax, uint32_t bankmax, void *buffer) {
    return stm_call(5, bufmax, bankmax, buffer, 0, 0x4393, 0);
}
static int sceCdStStart(uint32_t lba, const struct read_mode *mode) { return stm_call(1, lba, 0, 0, mode, 0x4393, 0); }
static int sceCdStSeekF(uint32_t lba) { return stm_call(9, lba, 0, 0, 0, 0x4393, 0); }
static int sceCdStSeek(uint32_t lba) { return stm_call(4, lba, 0, 0, 0, 0x4393, 0); }
static int sceCdStStop(void) { return stm_call(3, 0, 0, 0, 0, 0x4393, 0); }
static int sceCdStRead(uint32_t sectors, void *buffer, uint32_t mode, uint32_t *error) {
    (void)mode;
    return stm_call(1, 0, sectors, buffer, 0, 0x4394, error);
}
static int sceCdStPause(void) { return stm_call(7, 0, 0, 0, 0, 0x4393, 0); }
static int sceCdStResume(void) { return stm_call(8, 0, 0, 0, 0, 0x4393, 0); }
static int sceCdStStat(void) { return stm_call(6, 0, 0, 0, 0, 0x4393, 0); }
static int sceCdSetTimeout(int which, int ms) {
    if (which == 1) sync_timeout = ms;
    else if (which == 2) stream_timeout = ms;
    else return 0;
    return 1;
}

static int ret_zero(void) { return 0; }
static struct { uint32_t magic, next; uint16_t version, mode; char name[8] __attribute__((nonstring));
                uint32_t f[116]; } cdvdman = {OB_IRX_EXPORT_MAGIC, 0, 0x0101, 0, "cdvdman", {0}};
#define FN(p) ((uint32_t)(uintptr_t)(p))
void ob_iop_cdvdman_init(void) {
    cdvdman.f[0] = cdvdman.f[1] = cdvdman.f[2] = cdvdman.f[3] = FN(ret_zero);
    last_disk_type = CDVD_REG(0x0f);
    first_check = 1;
    stream_flag = 0; stream_cb[0] = stream_cb[1] = 0; poff_fn = 0;
    busy = 0; abort_read = 0; worker = -1; callback = 0; bounce = 0; last_error = 0;
    cdvdman.f[4] = FN(sceCdInit);       cdvdman.f[5] = FN(sceCdStandby);
    cdvdman.f[6] = FN(sceCdRead);       cdvdman.f[7] = FN(sceCdSeek);
    cdvdman.f[8] = FN(sceCdGetError);   cdvdman.f[9] = FN(sceCdGetToc);
    cdvdman.f[10] = FN(sceCdSearchFile); cdvdman.f[11] = FN(sceCdSync);
    cdvdman.f[12] = FN(sceCdGetDiskType); cdvdman.f[13] = FN(sceCdDiskReady);
    cdvdman.f[14] = FN(sceCdTrayReq);   cdvdman.f[15] = FN(sceCdStop);
    cdvdman.f[16] = FN(sceCdPosToInt);  cdvdman.f[17] = FN(sceCdIntToPos);
    cdvdman.f[21] = FN(sceCdCheckCmd);  cdvdman.f[22] = FN(sceCdRI);
    cdvdman.f[24] = FN(sceCdReadClock);
    cdvdman.f[28] = FN(sceCdStatus);
    cdvdman.f[29] = FN(sceCdApplySCmd);
    cdvdman.f[37] = FN(sceCdCallback);  cdvdman.f[38] = FN(sceCdPause);
    cdvdman.f[39] = FN(sceCdBreak);     cdvdman.f[40] = FN(sceCdReadCDDA);
    cdvdman.f[44] = FN(sceCdGetReadPos); cdvdman.f[46] = FN(sceCdNop);
    cdvdman.f[48] = FN(sceCdstm0Cb);    cdvdman.f[49] = FN(sceCdstm1Cb);
    cdvdman.f[50] = FN(sceCdSC);        cdvdman.f[51] = FN(sceCdRC);
    cdvdman.f[64] = FN(sceCdRM);
    cdvdman.f[56] = FN(sceCdStInit);    cdvdman.f[57] = FN(sceCdStRead);
    cdvdman.f[58] = FN(sceCdStSeek);    cdvdman.f[59] = FN(sceCdStStart);
    cdvdman.f[60] = FN(sceCdStStat);    cdvdman.f[61] = FN(sceCdStStop);
    cdvdman.f[67] = FN(sceCdStPause);   cdvdman.f[68] = FN(sceCdStResume);
    cdvdman.f[74] = FN(sceCdPowerOff);  cdvdman.f[75] = FN(sceCdMmode);
    cdvdman.f[77] = FN(sceCdStSeekF);   cdvdman.f[78] = FN(sceCdPOffCallback);
    cdvdman.f[79] = FN(sceCdReadDiskID); cdvdman.f[80] = FN(sceCdReadGUID);
    cdvdman.f[81] = FN(sceCdSetTimeout); cdvdman.f[82] = FN(sceCdReadModelID);
    cdvdman.f[83] = FN(sceCdReadDvdDualInfo);
    cdvdman.f[84] = FN(sceCdLayerSearchFile);
    cdvdman.f[114] = FN(sceCdRE);
    ob_iop_register_builtin((void *)&cdvdman, 115);
}
