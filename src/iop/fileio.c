/* SPDX-License-Identifier: MIT */
/* fileio: the EE file RPC server (0x80000001) on top of ioman, run in its own
   thread so device drivers may block. Two request layouts are served:

   - The ps2sdk layout: synchronous; the RPC reply carries the result.
   - The completion-record layout, selected when the client first calls
     GET_VERSION (0xff) with the EE address of a completion area. Requests
     start with {EE semaphore, EE result address, result size} and the RPC
     reply only acknowledges them. On completion, the record {semaphore,
     function, result address, result size, result, extras} is written to
     the completion area and SIF command 0x80000011 (opt = record slot,
     0x440 bytes each) tells the EE library to copy the result and signal
     the semaphore. Stat and dirent data use the 64-byte iomanX stat layout. */
#include "openbios/iop.h"
#include "openbios/ioman.h"
#include "openbios/sif.h"
#include "openbios/platform.h"

#define FIO_OPEN 0
#define FIO_CLOSE 1
#define FIO_READ 2
#define FIO_WRITE 3
#define FIO_LSEEK 4
#define FIO_IOCTL 5
#define FIO_REMOVE 6
#define FIO_MKDIR 7
#define FIO_RMDIR 8
#define FIO_DOPEN 9
#define FIO_DCLOSE 10
#define FIO_DREAD 11
#define FIO_GETSTAT 12
#define FIO_CHSTAT 13
#define FIO_FORMAT 14
#define FIO_DEVCTL 0x17
#define FIO_VERSION 0xff
#define CMD_FIO_DONE 0x80000011u
#define CHUNK 0x4000u

static uint32_t request_buffer[0xc20 / 4] __attribute__((aligned(64)));
static uint32_t reply[4] __attribute__((aligned(16)));
static uint8_t *chunk;
static uint32_t completion_area; /* EE address from GET_VERSION; 0: ps2sdk layout */
/* Completion record: 5 header words, then up to 0x41c bytes of extras. */
static uint32_t record[0x440 / 4] __attribute__((aligned(16)));
static uint32_t cmd_packet[4] __attribute__((aligned(16)));
static union { struct ob_io_stat stat; struct ob_io_dirent dirent; } scratch __attribute__((aligned(16)));

static uint32_t word(const void *p, unsigned offset) { return *(const uint32_t *)((const uint8_t *)p + offset); }
static int send_ee(const void *src, uint32_t dest, uint32_t bytes) {
    struct ob_sif0_segment seg;
    seg.iop_source = (uint32_t)(uintptr_t)src;
    seg.ee_destination = dest;
    seg.qwc = (bytes + 15) / 16;
    seg.irq = 0;
    return ob_iop_sif0_send_chain(&seg, 1);
}
/* NUL-terminate an incoming path field of the given capacity. */
static const char *path_at(void *buf, unsigned offset, unsigned capacity) {
    char *p = (char *)buf + offset;
    p[capacity - 1] = 0;
    return p;
}
/* READ: the 64-byte aligned middle of the EE buffer is written directly; the
   unaligned head and tail go into rem = {head size, tail size, head dest,
   tail dest, head[64], tail[64]}, which the EE library copies into place.
   The head is read on its own so every later chunk starts 64-aligned on the
   EE side too (SIF DMA refuses unaligned destinations). */
static int fio_read(int fd, uint32_t ptr, int size, uint32_t *rem) {
    uint32_t head, tail_start, middle_end, done = 0;
    if (size < 0) return -OB_IO_EINVAL;
    if (!chunk && !(chunk = ob_sysmem_alloc(OB_ALLOC_LAST, CHUNK, 0))) return -OB_IO_ENOMEM;
    head = (ptr & 63) ? 64 - (ptr & 63) : 0;
    if (head > (uint32_t)size) head = (uint32_t)size;
    tail_start = (ptr + (uint32_t)size) & ~63u;
    if (tail_start < ptr + head) tail_start = ptr + head;
    middle_end = tail_start - ptr;
    ob_zero(rem, (4 + 32) * 4);
    while (done < (uint32_t)size) {
        uint32_t want = (uint32_t)size - done, lo, hi;
        int got;
        if (want > CHUNK) want = CHUNK;
        if (done < head && want > head - done) want = head - done;
        got = ob_io_read(fd, chunk, (int)want);
        if (got < 0) { if (!done) return got; break; }
        if (!got) break;
        if (done < head) {
            hi = done + (uint32_t)got < head ? done + (uint32_t)got : head;
            ob_copy((uint8_t *)&rem[4] + done, chunk, hi - done);
        }
        lo = done > head ? done : head;
        hi = done + (uint32_t)got < middle_end ? done + (uint32_t)got : middle_end;
        if (hi > lo) send_ee(chunk + (lo - done), ptr + lo, hi - lo);
        lo = done > middle_end ? done : middle_end;
        hi = done + (uint32_t)got;
        if (hi > lo) ob_copy((uint8_t *)&rem[4 + 16] + (lo - middle_end), chunk + (lo - done), hi - lo);
        done += (uint32_t)got;
        if ((uint32_t)got < want) break;
    }
    rem[0] = head < done ? head : done;
    rem[1] = done > middle_end ? done - middle_end : 0;
    rem[2] = ptr;
    rem[3] = tail_start;
    return (int)done;
}
/* WRITE: the first mis bytes arrive in the request, the rest is fetched. */
static int fio_write(int fd, uint32_t ptr, uint32_t size, uint32_t mis, const uint8_t *head) {
    uint32_t done = 0;
    int r;
    if (!chunk && !(chunk = ob_sysmem_alloc(OB_ALLOC_LAST, CHUNK, 0))) return -OB_IO_ENOMEM;
    if (mis > 16) mis = 16;
    if (mis > size) mis = size;
    if (mis) {
        ob_copy(chunk, head, mis);
        r = ob_io_write(fd, chunk, (int)mis);
        if (r < 0) return r;
        done = (uint32_t)r;
        if (r < (int)mis) return (int)done;
    }
    while (done < size) {
        uint32_t n = size - done;
        if (n > CHUNK) n = CHUNK;
        if (ob_iop_get_other_data(ptr + done, chunk, (int)n) < 0) return done ? (int)done : -OB_IO_EIO;
        r = ob_io_write(fd, chunk, (int)n);
        if (r < 0) return done ? (int)done : r;
        done += (uint32_t)r;
        if ((uint32_t)r < n) break;
    }
    return (int)done;
}
/* ioman stat -> iomanX stat (64 bytes; type in the high mode bits). */
static void stat_x(const struct ob_io_stat *s, uint32_t *x) {
    ob_zero(x, 64);
    x[0] = (s->mode & 0x20) ? 0x116du : 0x2124u;
    x[1] = s->attr;
    x[2] = s->size;
    ob_copy(&x[3], s->ctime, 24);
    x[9] = s->hisize;
}

/* ---- ps2sdk layout ---- */
static void *sdk_request(int fno, void *buf, int size) {
    static uint32_t rem[4 + 32] __attribute__((aligned(16)));
    int r = -OB_IO_EINVAL;
    switch (fno) {
    case FIO_OPEN:
        /* {mode, name[256]} is always 260 bytes. Older SCE libraries
           (Gauntlet Dark Legacy) send {mode, arg, name} trimmed to the name,
           9 + strlen(name) bytes. */
        if (size == 4 + 256 || size < 9) r = ob_io_open(path_at(buf, 4, 256), (int)word(buf, 0));
        else r = ob_io_open(path_at(buf, 8, 0x400), (int)(word(buf, 0) & 0x0fffffffu));
        break;
    case FIO_CLOSE: r = ob_io_close((int)word(buf, 0)); break;
    case FIO_READ:
        r = fio_read((int)word(buf, 0), word(buf, 4), (int)word(buf, 8), rem);
        if (word(buf, 12)) send_ee(rem, word(buf, 12), sizeof(rem));
        break;
    case FIO_WRITE:
        r = fio_write((int)word(buf, 0), word(buf, 4), word(buf, 8), word(buf, 12), (const uint8_t *)buf + 16);
        break;
    case FIO_LSEEK: r = ob_io_lseek((int)word(buf, 0), (int)word(buf, 4), (int)word(buf, 8)); break;
    case FIO_IOCTL: r = ob_io_ioctl((int)word(buf, 0), (int)word(buf, 4), (uint8_t *)buf + 8); break;
    case FIO_REMOVE: r = ob_io_remove(path_at(buf, 0, 256)); break;
    case FIO_MKDIR: r = ob_io_mkdir(path_at(buf, 0, 256)); break;
    case FIO_RMDIR: r = ob_io_rmdir(path_at(buf, 0, 256)); break;
    case FIO_FORMAT: r = ob_io_format(path_at(buf, 0, 256)); break;
    case FIO_DOPEN: r = ob_io_dopen(path_at(buf, 0, 256)); break;
    case FIO_DCLOSE: r = ob_io_dclose((int)word(buf, 0)); break;
    case FIO_DREAD:
        ob_zero(&scratch, sizeof(scratch));
        r = ob_io_dread((int)word(buf, 0), &scratch.dirent);
        if (r > 0 && word(buf, 4)) send_ee(&scratch.dirent, word(buf, 4), sizeof(scratch.dirent));
        break;
    case FIO_GETSTAT:
        ob_zero(&scratch, sizeof(scratch));
        r = ob_io_getstat(path_at(buf, 4, 256), &scratch.stat);
        if (r >= 0 && word(buf, 0)) send_ee(&scratch.stat, word(buf, 0), sizeof(scratch.stat));
        break;
    case FIO_CHSTAT:
        ob_copy(&scratch.stat, (uint8_t *)buf + 4, sizeof(scratch.stat));
        r = ob_io_chstat(path_at(buf, 4 + sizeof(scratch.stat), 256), &scratch.stat, word(buf, 0));
        break;
    default:
        ob_puts("[openbios:iop] fileio unsupported fno="); ob_puthex((uint32_t)fno); ob_puts("\n");
        break;
    }
    reply[0] = (uint32_t)r;
    return reply;
}

/* ---- completion-record layout ---- */
static void *record_request(int fno, void *buf) {
    uint32_t extra = 0; /* bytes of extras after the 5 header words */
    uint32_t head[3] = {word(buf, 0), word(buf, 4), word(buf, 8)};
    int r = -OB_IO_EINVAL;
    if (fno == FIO_READ) {
        /* Acknowledge before reading, like the real fileio: the EE caller
           only waits for the reply and gets the result through the
           completion record. Enter the Matrix's movie player calls this from
           its decoding thread with 1 MiB reads, and stuttered while a reply
           waited for the whole read. The next request may land in buf once
           the reply is out, so take its words first. */
        int fd = (int)word(buf, 0xc), size = (int)word(buf, 0x14);
        uint32_t ptr = word(buf, 0x10);
        reply[0] = 1;
        ob_iop_rpc_reply_early(reply);
        r = fio_read(fd, ptr, size, &record[5]);
        extra = (4 + 32) * 4;
        goto done;
    }
    switch (fno) {
    case FIO_OPEN: r = ob_io_open(path_at(buf, 0x14, 0x400), (int)word(buf, 0xc)); break;
    case FIO_CLOSE: r = ob_io_close((int)word(buf, 0xc)); break;
    case FIO_WRITE:
        r = fio_write((int)word(buf, 0xc), word(buf, 0x10), word(buf, 0x14), word(buf, 0x18),
                      (const uint8_t *)buf + 0x1c);
        break;
    case FIO_LSEEK: r = ob_io_lseek((int)word(buf, 0xc), (int)word(buf, 0x10), (int)word(buf, 0x14)); break;
    case FIO_IOCTL: r = ob_io_ioctl((int)word(buf, 0xc), (int)word(buf, 0x10), (uint8_t *)buf + 0x14); break;
    case FIO_REMOVE: r = ob_io_remove(path_at(buf, 0xc, 0x400)); break;
    case FIO_MKDIR: r = ob_io_mkdir(path_at(buf, 0x10, 0x400)); break;
    case FIO_RMDIR: r = ob_io_rmdir(path_at(buf, 0xc, 0x400)); break;
    case FIO_DOPEN: r = ob_io_dopen(path_at(buf, 0xc, 0x400)); break;
    case FIO_DCLOSE: r = ob_io_dclose((int)word(buf, 0xc)); break;
    case FIO_DREAD: /* extras: {EE dirent, stat[64], name[256]} */
        ob_zero(&scratch, sizeof(scratch));
        r = ob_io_dread((int)word(buf, 0xc), &scratch.dirent);
        record[5] = word(buf, 0x10);
        stat_x(&scratch.dirent.stat, &record[6]);
        ob_copy(&record[6 + 16], scratch.dirent.name, 256);
        extra = 4 + 64 + 256;
        break;
    case FIO_GETSTAT: /* extras: {EE stat, stat[64]} */
        ob_zero(&scratch, sizeof(scratch));
        r = ob_io_getstat(path_at(buf, 0x10, 0x400), &scratch.stat);
        record[5] = word(buf, 0xc);
        stat_x(&scratch.stat, &record[6]);
        extra = 4 + 64;
        break;
    case FIO_FORMAT: r = ob_io_format(path_at(buf, 0xc, 0x400)); break;
    case FIO_DEVCTL: {
        /* {name[0x400], arg[0x400], cmd, arglen, EE buf, buflen}, as seen from
           Monster Hunter's network tool (devctl "dev9x:" 0x4402 to power the
           adapter off on exit). The buf/buflen words are a guess so far, only
           calls without an output buffer have been seen. */
        static uint8_t out[0x400] __attribute__((aligned(16)));
        static int reported;
        uint32_t arglen = word(buf, 0x810), ee_buf = word(buf, 0x814), buflen = word(buf, 0x818);
        if (arglen > 0x400) arglen = 0x400;
        if (buflen > sizeof(out)) buflen = sizeof(out);
        ob_zero(out, buflen);
        r = ob_io_devctl(path_at(buf, 0xc, 0x400), (int)word(buf, 0x80c), (uint8_t *)buf + 0x40c,
                         arglen, buflen ? out : 0, buflen);
        if (buflen && r >= 0) {
            if (ee_buf && !(ee_buf & 15)) send_ee(out, ee_buf, buflen);
            else if (!reported) {
                reported = 1;
                ob_puts("[openbios:iop] fileio devctl: output buffer not sent\n");
            }
        }
        break;
    }
    default:
        ob_puts("[openbios:iop] fileio unsupported fno="); ob_puthex((uint32_t)fno);
#ifdef OB_TRACE_RPC
        { unsigned i; for (i = 0; i < 0x40; i += 4) { ob_puts(" "); ob_puthex(word(buf, i)); }
          ob_puts(" str@0x14="); ob_puts(path_at(buf, 0x14, 0x40)); }
#endif
        ob_puts("\n");
        break;
    }
#ifdef OB_TRACE_RPC
    ob_puts("[openbios:fio] fno="); ob_puthex((uint32_t)fno); ob_puts(" r="); ob_puthex((uint32_t)r);
    if (fno == FIO_OPEN) { ob_puts(" "); ob_puts(path_at(buf, 0x14, 0x400)); }
    if (fno == FIO_READ) { ob_puts(" n="); ob_puthex(word(buf, 0x14)); }
    ob_puts("\n");
#endif
done:
    record[0] = head[0];          /* EE semaphore */
    record[1] = (uint32_t)fno;
    record[2] = head[1];          /* EE result address */
    record[3] = head[2];          /* result size */
    record[4] = (uint32_t)r;
    if (record[3] > 4) record[3] = 4;
    cmd_packet[3] = 0;            /* record slot 0: the first area GET_VERSION named */
    if (!ob_iop_send_cmd(CMD_FIO_DONE, cmd_packet, 16, record, completion_area, (int)(20 + extra)))
        ob_puts("[openbios:iop] fileio completion send failed\n");
    reply[0] = 1;                 /* accepted */
    return reply;
}

static void *fileio_server(int fno, void *buf, int size) {
    if (fno == FIO_VERSION) {
        /* {completion slot 0, slot 1} -> {protocol version, remainder mode 2}. */
        completion_area = word(buf, 0);
        ob_copy(reply, ob_iop_loadfile_version, 4);
        reply[1] = 2;
        return reply;
    }
    return completion_area ? record_request(fno, buf) : sdk_request(fno, buf, size);
}
void ob_iop_fileio_init(void) {
    /* Allocate the transfer buffer before modules load; clients may take
       nearly all IOP memory before reading files. */
    chunk = ob_sysmem_alloc(OB_ALLOC_LAST, CHUNK, 0);
    completion_area = 0;
    ob_iop_rpc_server_start(0x80000001, fileio_server, request_buffer, 0x58, 0x1000);
}
