/* SPDX-License-Identifier: MIT */
/* Native IOP RPC endpoint: answers BIND and CALL from the EE with RPC_END over
   SIF0, for servers implemented here. This is not an IRX-loadable sifrpc. */
#include "openbios/sif.h"
#include "openbios/platform.h"
#include "openbios/iop.h"
#include "openbios/ioman.h"
#include "openbios/rom.h"
#include "openbios/disc.h"

#define RPC_AREA_IOP 0xa0088800u   /* Server buffers, 1 KiB each (up to the hole end). */
#define RPC_BUFFER_SIZE 0x400u
#define REPLY_DATA_IOP 0xa0081100u /* Reply payload staging before RPC_END. */
#define REPLY_DATA_MAX 0xe00u

typedef uint32_t (*rpc_fn)(uint32_t number, const uint8_t *in, uint32_t in_size,
                           uint8_t *out, uint32_t out_size);
struct server { uint32_t sid; rpc_fn call; };

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
/* sceCdInit (public libcdvd): reply {result, fsv version, man version, verbose}. */
static uint32_t cd_init(uint32_t number, const uint8_t *in, uint32_t in_size,
                        uint8_t *out, uint32_t out_size) {
    (void)number;
    ob_puts("[openbios:iop] cdvd init mode=");
    ob_puthex(in_size >= 4 ? ob_le32(in) : 0xffffffffu); ob_puts("\n");
    if (out_size < 16) return 0;
    put32(out, 1);
    /* cdvdfsv/cdvdman versions: above 0x104 selects the 64-byte remainder
       packet layout that cd_read_ee implements. */
    put32(out + 4, 0x0226);
    put32(out + 8, 0x0226);
    return 16;
}
/* IOP heap (SifAllocIopHeap/SifFreeIopHeap): fno 1 alloc(size) -> addr,
   fno 2 free(addr) -> result, fno 3 {addr, path} loads a whole file to addr
   (SifLoadIopHeap), fno 4 {size, mode, addr} -> addr allocates with a sysmem
   mode, fno 5 frees such a block. */
static uint32_t heap_alloc(uint32_t size) {
    return (uint32_t)(uintptr_t)ob_sysmem_alloc(OB_ALLOC_FIRST, size, 0);
}
static int heap_free(uint32_t addr) {
    return ob_sysmem_free((void *)(uintptr_t)addr) == KE_OK ? 0 : -1;
}
/* Monster Hunter's network setup (EzMIDI) loads its sound bank this way. The
   caller sized the block from the file, so just read until EOF. Returns 0 or
   a negative ioman error; retail's exact error codes aren't checked yet. */
static int heap_load(uint32_t addr, const uint8_t *in, uint32_t in_size) {
    static char path[256]; /* only the SIF thread gets here */
    uint32_t n;
    int fd, got;
    for (n = 0; n + 1 < sizeof(path) && n < in_size && in[n]; ++n) path[n] = (char)in[n];
    path[n] = 0;
    if (!n || !addr) return -OB_IO_EINVAL;
    fd = ob_io_open(path, OB_IO_O_RDONLY);
    if (fd < 0) return fd;
    while ((got = ob_io_read(fd, (void *)(uintptr_t)addr, 0x10000)) > 0) addr += (uint32_t)got;
    ob_io_close(fd);
    return got < 0 ? got : 0;
}
static uint32_t iop_heap(uint32_t number, const uint8_t *in, uint32_t in_size,
                         uint8_t *out, uint32_t out_size) {
    uint32_t value = in_size >= 4 ? ob_le32(in) : 0, result;
    if (out_size < 4) return 0;
    if (number == 1) result = heap_alloc(value);
    else if (number == 2 || number == 5) result = (uint32_t)heap_free(value);
    else if (number == 3 && in_size > 4) result = (uint32_t)heap_load(value, in + 4, in_size - 4);
    else if (number == 6) result = ob_sysmem_largest_free(); /* QueryMaxFreeMemSize */
    else if (number == 7) result = ob_sysmem_free_size();    /* QueryTotalFreeMemSize */
    else if (number == 4 && in_size >= 12 && ob_le32(in + 4) <= OB_ALLOC_ADDRESS)
        result = (uint32_t)(uintptr_t)ob_sysmem_alloc((int)ob_le32(in + 4), value,
                                                      ob_le32(in + 8));
    else {
        unsigned k;
        ob_puts("[openbios:iop] iopheap unsupported fno="); ob_puthex(number);
        ob_puts(" size="); ob_puthex(in_size); ob_puts(" out="); ob_puthex(out_size);
        for (k = 0; k < 4 && 4 * k + 4 <= in_size; ++k) { ob_puts(" "); ob_puthex(ob_le32(in + 4 * k)); }
        ob_puts("\n");
        result = (uint32_t)-1;
    }
    if (number == 1 && !result) {
        ob_puts("[openbios:iop] iopheap free="); ob_puthex(ob_sysmem_free_size()); ob_puts("\n");
        ob_sysmem_dump();
    }
    ob_puts("[openbios:iop] iopheap fno="); ob_puthex(number); ob_puts(" arg=");
    ob_puthex(value); ob_puts(" ret="); ob_puthex(result); ob_puts("\n");
    put32(out, result);
    return 4;
}
static void log_text(const uint8_t *p, uint32_t max) {
    char text[80];
    uint32_t n;
    for (n = 0; n < max && n + 1 < sizeof(text) && p[n]; ++n)
        text[n] = (char)(p[n] >= 32 && p[n] < 127 ? p[n] : '.');
    text[n] = 0;
    ob_puts(text);
}
/* Map "rom0:NAME" to a ROM directory entry. Other devices are not served yet. */
static int ob_rom_name_from_path(const char *path, struct ob_rom_file *out) {
    char name[11];
    unsigned i;
    if (path[0] != 'r' || path[1] != 'o' || path[2] != 'm' || path[3] != '0' || path[4] != ':')
        return OB_ENOTFOUND;
    path += 5;
    if (*path == '\\' || *path == '/') ++path;
    for (i = 0; i < 10 && path[i] && path[i] != ';'; ++i) name[i] = path[i];
    if (path[i] && path[i] != ';') return OB_ENOTFOUND;
    name[i] = 0;
    return ob_rom_find((const void *)0xbfc00000u, OB_ROM_SIZE, name, out);
}
char ob_iop_loadfile_version[4] = {'3', '1', '0', '0'};
/* LOADFILE (public _lf_module_load_arg: {arg_len/result, modres, path[252],
   args[252]}). Modules load from rom0:; other devices fail with KE_NOFILE. */
static uint32_t load_file(uint32_t number, const uint8_t *in, uint32_t in_size,
                          uint8_t *out, uint32_t out_size) {
    if (number == 0xff && out_size >= 4) {
        /* GET_VERSION: a 4-character protocol version that clients compare
           exactly; see ob_iop_loadfile_version. */
        ob_copy(out, ob_iop_loadfile_version, 4);
        return 4;
    }
    if (number == 7 && in_size >= 8 + 252 && out_size >= 4) {
        /* STOP: {id, arg length, path area, args} -> {result, module result} */
        int modres = 0, r = ob_iop_stop_module((int)ob_le32(in), (const char *)in + 8 + 252,
                                               ob_le32(in + 4) > 252 ? 252 : ob_le32(in + 4), &modres);
        put32(out, (uint32_t)r);
        if (out_size < 8) return 4;
        put32(out + 4, (uint32_t)modres);
        return 8;
    }
    if (number == 8 && in_size >= 4 && out_size >= 4) { /* UNLOAD: {id} -> {result} */
        put32(out, (uint32_t)ob_iop_unload_module((int)ob_le32(in)));
        return 4;
    }
    if (number == 9 && in_size >= 8 + 252 && out_size >= 4) { /* SEARCH BY NAME: name at +8 */
        char name[253];
        ob_copy(name, in + 8, 252);
        name[252] = 0;
        put32(out, (uint32_t)ob_iop_module_search(name, 0));
        return 4;
    }
    if (number == 10 && in_size >= 4 && out_size >= 4) { /* SEARCH BY ADDRESS */
        put32(out, (uint32_t)ob_iop_module_search(0, ob_le32(in)));
        return 4;
    }
    if (number == 6 && in_size >= 8 + 252 && out_size >= 4) {
        /* MOD_BUF_LOAD (SifExecModuleBuffer): the module image is already in
           IOP memory at the first word (an IOP heap block the EE filled); the
           argument length is the second word and the args sit at +260. */
        uint32_t address = ob_le32(in) & 0x1fffffu, args_len = ob_le32(in + 4), block, size = 0;
        int modres = 0, id;
        if (args_len > 252) args_len = 252;
        block = ob_sysmem_block(address, &size);
        size = block ? block + size - address : (address < 0x200000u ? 0x200000u - address : 0);
        id = size ? ob_iop_load_module((const void *)(uintptr_t)(0xa0000000u | address), size,
                                       "buffer", (const char *)in + 8 + 252, args_len, &modres)
                  : KE_ILLEGAL_OBJECT;
        if (id < 0) {
            ob_puts("[openbios:iop] loadfile buffer "); ob_puthex(address); ob_puts(" failed error=");
            ob_puthex((uint32_t)id); ob_puts("\n");
        }
        put32(out, (uint32_t)id);
        if (out_size < 8) return 4;
        put32(out + 4, (uint32_t)modres);
        return 8;
    }
    /* Older libraries ask for the result word only (4-byte reply). */
    if ((number == 0 || number == 4) && in_size >= 8 + 252 && out_size >= 4) {
        /* MOD_LOAD: path at +8 (252 bytes), NUL-separated args at +260,
           argument length in the first word. */
        char path[253];
        struct ob_rom_file rom;
        int modres = 0, id;
        uint32_t args_len = ob_le32(in);
        ob_copy(path, in + 8, 252);
        path[252] = 0;
        if (args_len > 252) args_len = 252;
#ifdef OB_TRACE_RPC
        ob_puts("[openbios:iop] loadfile "); ob_puts(path); ob_puts(" args="); ob_puthex(args_len); ob_puts("\n");
#endif
        if (path[0] == 'r' && path[1] == 'o' && path[2] == 'm' && path[3] == '0' && path[4] == ':') {
            id = ob_rom_name_from_path(path, &rom) ? KE_NOFILE :
                ob_iop_load_module(rom.data, rom.size, path, (const char *)in + 8 + 252,
                                   args_len, &modres);
        } else if (path[0] == 'c' && path[1] == 'd' && path[2] == 'r' && path[3] == 'o' &&
                   path[4] == 'm') {
            void *file;
            uint32_t size;
            if (!ob_iop_cd_load_file(path, &file, &size)) {
                /* Free the file before the module starts: start routines often
                   take most of the remaining memory in one block. */
                id = ob_iop_load_module_only(file, size, path);
                ob_sysmem_free(file);
                if (id >= 0)
                    id = ob_iop_start_module(id, path, (const char *)in + 8 + 252, args_len, &modres);
            } else id = KE_NOFILE;
        } else id = KE_NOFILE; /* Other devices are not served yet. */
        if (id < 0) {
            ob_puts("[openbios:iop] loadfile "); ob_puts(path); ob_puts(" failed error=");
            ob_puthex((uint32_t)id); ob_puts("\n");
        }
        put32(out, (uint32_t)id);
        if (out_size < 8) return 4;
        put32(out + 4, (uint32_t)modres);
        return 8;
    }
    ob_puts("[openbios:iop] loadfile fno="); ob_puthex(number);
    if (in_size >= 8 + 252 && (number == 0 || number == 4 || number == 1 || number == 5)) {
        ob_puts(" path="); log_text(in + 8, 252);
        if (number == 0 || number == 4) { ob_puts(" args="); log_text(in + 8 + 252, 252); }
    }
    ob_puts(" -> unsupported\n");
    if (out_size < 8) return 0;
    put32(out, (uint32_t)-1);
    put32(out + 4, 0);
    return 8;
}
/* CDVD S-commands (public libcdvd numbering). Replies start with a result word.
   Values come from the drive registers or the raw S-command interface. */
#define CDVD_REG(n) (*(volatile uint8_t *)(uintptr_t)(0xbf402000u + (n)))
static uint32_t cd_scmd(uint32_t number, const uint8_t *in, uint32_t in_size,
                        uint8_t *out, uint32_t out_size) {
    uint32_t arg = in_size >= 4 ? ob_le32(in) : 0, result = 1, bytes = 4;
    uint8_t r[16];
    if (out_size < 4) return 0;
    switch (number) {
    case 1: /* READCLOCK: S-command 0x08 -> status, then 7 clock bytes. */
        ob_zero(r, sizeof(r));
        if (ob_cd_scommand(0x08, 0, 0, r, 8) < 8 || out_size < 12) { result = 0; break; }
        ob_copy(out + 4, r, 8);
        bytes = 12;
        break;
    case 3: put32(out, CDVD_REG(0x0f)); return 4;   /* GETDISKTYPE */
    case 4: put32(out, CDVD_REG(0x06)); return 4;   /* GETERROR */
    case 5: /* TRAYREQ: report no tray change. */
        if (out_size >= 8) { put32(out + 4, 0); bytes = 8; }
        break;
    case 12: put32(out, CDVD_REG(0x0a)); return 4;  /* STATUS */
    case 0x27: { /* READ DVD DUAL INFO: {result, on_dual, layer 1 start} */
        int dual;
        uint32_t layer1;
        if (out_size < 12) break;
        ob_iop_cd_layer_info(&dual, &layer1);
        put32(out + 4, (uint32_t)dual);
        put32(out + 8, layer1);
        bytes = 12;
        break;
    }
    case 34: /* MMODE: media mode only affects later reads here. */
    case 35: /* SETTHREADPRI */
        break;
    default:
        ob_puts("[openbios:iop] cdvd scmd unsupported fno="); ob_puthex(number);
        ob_puts(" arg="); ob_puthex(arg); ob_puts("\n");
        result = 0;
    }
    put32(out, result);
    return bytes;
}
/* sceCdDiskReady (public cdvdman semantics): mode 8 returns the raw N-command
   status register; mode 0 waits while the drive is still detecting the disc;
   otherwise 2 (SCECdComplete) when ready and 6 (SCECdNotReady) when not. */
static uint32_t cd_disk_ready(uint32_t number, const uint8_t *in, uint32_t in_size,
                              uint8_t *out, uint32_t out_size) {
    uint32_t mode = in_size >= 4 ? ob_le32(in) : 1, result;
    unsigned i;
    (void)number;
    if (out_size < 4) return 0;
    if (mode == 8) result = CDVD_REG(0x05);
    else if (mode == 0) {
        for (i = 0; i < 2000000; ++i) {
            uint8_t type = CDVD_REG(0x0f);
            if (type < 1 || type > 4 || (CDVD_REG(0x05) & 0xc0) == 0x40) break;
        }
        result = 2;
    } else result = (CDVD_REG(0x05) & 0xc0) == 0x40 ? 2 : 6;
    put32(out, result);
    return 4;
}
/* sceCdSearchFile (public cdvdfsv_rpc4 packets): request sizes 0x12c/0x128/0x124
   select the layout. The sceCdlFILE (lsn, size, name[16], date[8]) is filled in
   place and DMA'd to the EE destination; the reply is {result, pad[3]}. */
static uint32_t cd_search(uint32_t number, const uint8_t *in, uint32_t in_size,
                          uint8_t *out, uint32_t out_size) {
    uint8_t *packet = (uint8_t *)in;
    uint32_t path_offset = in_size == 0x124 ? 32 : 36;
    uint32_t dest_offset = path_offset + 256, send = in_size == 0x124 ? 32 : 36;
    uint32_t lba = 0, size = 0, dest, i, start = 0;
    char path[257];
    int found;
    (void)number;
    if (in_size < 0x124 || out_size < 4) return 0;
    ob_copy(path, packet + path_offset, 256);
    path[256] = 0;
    found = !ob_iop_cd_find(path, &lba, &size);
    ob_zero(packet, 32);
    if (found) {
        put32(packet, lba);
        put32(packet + 4, size);
        for (i = 0; path[i]; ++i) if (path[i] == '\\' || path[i] == '/' || path[i] == ':') start = i + 1;
        for (i = 0; i < 15 && path[start + i]; ++i) packet[8 + i] = (uint8_t)path[start + i];
        /* Recording date is not reported yet (zeros). */
    } else {
        ob_puts("[openbios:iop] cdvd search not found: "); ob_puts(path);
        ob_puts(" (request size "); ob_puthex(in_size); ob_puts(")\n");
#ifdef OB_TRACE_RPC
        { unsigned k; for (k = 0; k < 48; k += 4) { ob_puts(" "); ob_puthex(ob_le32(in + k)); } ob_puts("\n"); }
#endif
    }
    dest = ob_le32(packet + dest_offset);
    if (dest) {
        struct ob_sif0_segment seg = {(uint32_t)(uintptr_t)packet, dest, (send + 15) / 16, 0};
        ob_iop_sif0_send_chain(&seg, 1);
    }
    ob_zero(out, 16);
    put32(out, (uint32_t)found);
    return 16;
}

/* N-commands (public libcdvd numbering). READ/DVDREAD packet: lbn, sectors,
   EE buffer, read mode, EE remainder packet address, EE read-position address.
   The EE buffer's unaligned head and tail (to 64 bytes) travel in a remainder
   packet {head len, tail len, head dst, tail dst, head[64], tail[64]} that the
   EE copies in its completion callback; the aligned middle is DMA'd directly. */
#define READ_CHUNK_SECTORS 8u /* two 16 KiB buffers; IOP RAM is scarce */
static uint8_t *read_buffers[2];
static uint32_t remainder[4 + 32] __attribute__((aligned(16)));
static uint32_t read_position[4] __attribute__((aligned(16)));
static int send_ee(const void *src, uint32_t dest, uint32_t bytes) {
    struct ob_sif0_segment seg;
    seg.iop_source = (uint32_t)(uintptr_t)src;
    seg.ee_destination = dest;
    seg.qwc = (bytes + 15) / 16;
    seg.irq = 0;
    return ob_iop_sif0_send_chain(&seg, 1);
}
struct ee_read {
    uint32_t paddr, head, middle_end, position_dest, done;
    uint32_t carry_n;   /* bytes held back from the previous piece */
    uint8_t carry[16];
};
/* Route one streamed piece (sectors [first, first + n) of the request) to the
   head remainder, the EE buffer or the tail remainder, then publish progress
   in bytes like cdvdfsv.
   EE DMA wants 16-byte aligned destinations. Only the first piece starts on
   one; after that pieces land at paddr + k * 16 KiB, so with an odd paddr the
   SIF send refused every piece but the first and the EE got a buffer with
   holes (R&C Size Matters reads its text that way: no menu text at all). The
   last few bytes of each piece are held back and sent in front of the next,
   using the read buffers' 64 bytes of slack. The middle ends 64-aligned, so
   nothing is left over at the end. */
static int ee_read_sink(void *ctx, const uint8_t *data, uint32_t first, uint32_t n) {
    struct ee_read *r = ctx;
    uint32_t chunk_start = first * 2048, chunk_end = chunk_start + n * 2048, lo, hi;
    if (chunk_start < r->head) {
        hi = chunk_end < r->head ? chunk_end : r->head;
        ob_copy((uint8_t *)&remainder[4] + chunk_start, data, hi - chunk_start);
    }
    /* Copy the tail out first, the middle below shifts this piece's data. */
    lo = chunk_start > r->middle_end ? chunk_start : r->middle_end;
    if (chunk_end > lo)
        ob_copy((uint8_t *)&remainder[4 + 16] + (lo - r->middle_end), data + (lo - chunk_start), chunk_end - lo);
    lo = chunk_start > r->head ? chunk_start : r->head;
    hi = chunk_end < r->middle_end ? chunk_end : r->middle_end;
    if (hi > lo) {
        uint8_t *src = (uint8_t *)data + (lo - chunk_start); /* our own read buffer */
        uint32_t bytes = hi - lo, keep = (r->paddr + hi) & 15, i;
        static int reported;
        if (r->carry_n) {
            for (i = bytes; i-- > 0;) src[i + r->carry_n] = src[i];
            ob_copy(src, r->carry, r->carry_n);
            bytes += r->carry_n;
        }
        if (bytes > keep && send_ee(src, r->paddr + hi - bytes, bytes - keep) && !reported) {
            reported = 1;
            ob_puts("[openbios:iop] cdvd read: EE transfer refused, dest=");
            ob_puthex(r->paddr + hi - bytes); ob_puts("\n");
        }
        ob_copy(r->carry, src + bytes - keep, keep);
        r->carry_n = keep;
    }
    r->done = chunk_end;
    if (r->position_dest) {
        read_position[0] = chunk_end < r->middle_end ? chunk_end : r->middle_end;
        send_ee(read_position, r->position_dest, 16);
    }
    return 0;
}
static uint32_t cd_read_ee(const uint8_t *in, uint8_t *out) {
    uint32_t lbn = ob_le32(in), sectors = ob_le32(in + 4), paddr = ob_le32(in + 8);
    uint32_t remainder_dest = ob_le32(in + 16), total = sectors * 2048, tail_start;
    uint8_t mode_pattern = in[14];
    struct ee_read r;
    volatile uint32_t detail;
#ifdef OB_TRACE_RPC
    ob_puts("[openbios:rpc] cd read lbn="); ob_puthex(lbn); ob_puts(" n="); ob_puthex(sectors);
    ob_puts(" ee="); ob_puthex(paddr); ob_puts(" mode="); ob_puthex(ob_le32(in + 12)); ob_puts("\n");
#endif
    if (mode_pattern != 0) {
        ob_puts("[openbios:iop] cdvd read: only 2048-byte sectors supported, pattern=");
        ob_puthex(mode_pattern); ob_puts("\n");
    }
    if (!read_buffers[0]) {
        read_buffers[0] = ob_sysmem_alloc(OB_ALLOC_LAST, READ_CHUNK_SECTORS * 2048 + 64, 0);
        read_buffers[1] = ob_sysmem_alloc(OB_ALLOC_LAST, READ_CHUNK_SECTORS * 2048 + 64, 0);
    }
    if (!read_buffers[0] || !read_buffers[1] || !sectors) { put32(out, 0); return 4; }
    r.paddr = paddr;
    r.head = (paddr & 63) ? 64 - (paddr & 63) : 0;
    if (r.head > total) r.head = total;
    tail_start = (paddr + total) & ~63u;
    if (tail_start < paddr + r.head) tail_start = paddr + r.head;
    r.middle_end = tail_start - paddr;
    r.position_dest = ob_le32(in + 20);
    r.done = 0;
    r.carry_n = 0;
    ob_zero(remainder, sizeof(remainder));
    ob_iop_cd_read_stream(lbn, sectors, read_buffers, READ_CHUNK_SECTORS, ee_read_sink, &r, &detail);
    remainder[0] = r.head;
    remainder[1] = paddr + total - tail_start;
    remainder[2] = paddr;
    remainder[3] = tail_start;
    if (remainder_dest) send_ee(remainder, remainder_dest, sizeof(remainder));
    read_position[0] = r.done;
    if (r.position_dest) send_ee(read_position, r.position_dest, 16);
    put32(out, r.done);
    return 4;
}
/* CD streaming (public libcdvd sceCdStream packet {lbn, sectors, buffer,
   command, mode}). Data is read on demand at the stream position with one
   drive command per request and sent straight to the EE, so every READ finds
   the requested sectors "buffered"; STAT reports a full buffer. */
static struct { int active; uint32_t lbn, buffer_sectors; } stream;
struct stream_sink { uint32_t ee, sent; };
static int stream_sink(void *ctx, const uint8_t *data, uint32_t first, uint32_t n) {
    struct stream_sink *k = ctx;
    send_ee(data, k->ee + first * 2048, n * 2048);
    k->sent = first + n;
    return 0;
}
static uint32_t cd_stream(const uint8_t *in) {
    uint32_t lbn = ob_le32(in), sectors = ob_le32(in + 4), ee = ob_le32(in + 8), cmd = ob_le32(in + 12);
#ifdef OB_TRACE_RPC
    ob_puts("[openbios:rpc] stream cmd="); ob_puthex(cmd); ob_puts(" lbn="); ob_puthex(lbn);
    ob_puts(" n="); ob_puthex(sectors); ob_puts(" ee="); ob_puthex(ee); ob_puts(" pos="); ob_puthex(stream.lbn); ob_puts("\n");
#endif
    switch (cmd) {
    case 1: stream.active = 1; stream.lbn = lbn; return 1;               /* START */
    case 2: {                                                            /* READ */
        struct stream_sink k = {ee, 0};
        volatile uint32_t detail;
        int r;
        if (!stream.active || !sectors) return 0;
        if (!read_buffers[0]) {
            read_buffers[0] = ob_sysmem_alloc(OB_ALLOC_LAST, READ_CHUNK_SECTORS * 2048 + 64, 0);
            read_buffers[1] = ob_sysmem_alloc(OB_ALLOC_LAST, READ_CHUNK_SECTORS * 2048 + 64, 0);
            if (!read_buffers[0] || !read_buffers[1]) return 0;
        }
        if (sectors > 0xffff) sectors = 0xffff;
        r = ob_iop_cd_read_stream(stream.lbn, sectors, read_buffers, READ_CHUNK_SECTORS,
                                  stream_sink, &k, &detail);
        stream.lbn += k.sent;
#ifdef OB_TRACE_RPC
        ob_puts("[openbios:rpc] stream read r="); ob_puthex((uint32_t)r); ob_puts(" sent="); ob_puthex(k.sent);
        ob_puts(" detail="); ob_puthex(detail); ob_puts("\n");
#endif
        return k.sent | (r ? (uint32_t)(detail & 0xff) << 16 : 0);
    }
    case 3: stream.active = 0; return 1;                                 /* STOP */
    case 4: case 9: stream.lbn = lbn; return 1;                          /* SEEK, SEEKF */
    case 5: stream.buffer_sectors = lbn; stream.active = 0; return 1;    /* INIT */
    case 6: return stream.active ? stream.buffer_sectors : 0;            /* STAT */
    case 7: case 8: return 1;                                            /* PAUSE, RESUME */
    default: return 0;
    }
}
static uint32_t cd_ncmd(uint32_t number, const uint8_t *in, uint32_t in_size,
                        uint8_t *out, uint32_t out_size) {
    if (out_size < 4) out_size = 4;
    ob_zero(out, out_size < 32 ? out_size : 32);
    switch (number) {
    case 1: case 3: /* READ, DVDREAD */
        if (in_size < 24) break;
        return cd_read_ee(in, out);
    case 5: case 6: case 7: case 8: /* SEEK, STANDBY, STOP, PAUSE: drive state only */
        put32(out, 1);
        return 4;
    case 9: /* STREAM */
        if (in_size < 16) break;
        put32(out, cd_stream(in));
        return 4;
    case 14: /* DISKREADY */
        put32(out, (CDVD_REG(0x05) & 0xc0) == 0x40 ? 2 : 6);
        return 4;
    default:
        break;
    }
    ob_puts("[openbios:iop] cdvd ncmd unsupported fno="); ob_puthex(number);
    ob_puts(" size="); ob_puthex(in_size); ob_puts("\n");
    put32(out, 0);
    return 4;
}
/* The CDVD services run on their own threads so a long read never blocks the
   thread that delivers every other RPC. N-commands get one thread; the quick
   services share a second. */
#define CD_SERVERS 6
/* stack 0: shares the thread of the server listed before it. */
static const struct cd_server { uint32_t sid; rpc_fn call; uint32_t stack; } cd_servers[CD_SERVERS] = {
    {0x80000595u, cd_ncmd, 0x1000},
    {0x80000597u, cd_search, 0x2000}, /* ISO directory walks need the room */
    {0x8000059au, cd_disk_ready, 0},
    {0x8000059cu, cd_disk_ready, 0},
    {0x80000593u, cd_scmd, 0},
    {0x80000592u, cd_init, 0},
};
static struct {
    uint8_t in[RPC_BUFFER_SIZE] __attribute__((aligned(64)));
    uint8_t out[0x100] __attribute__((aligned(64)));
} cd_buffers[CD_SERVERS];
static void *cd_server(int fno, void *buf, int size) {
    unsigned i;
    for (i = 0; i < CD_SERVERS && buf != cd_buffers[i].in; ++i) {}
    if (i == CD_SERVERS) return 0;
    if (size < 0) size = 0;
    if (size > (int)RPC_BUFFER_SIZE) size = RPC_BUFFER_SIZE;
    ob_zero(cd_buffers[i].out, sizeof(cd_buffers[i].out));
    cd_servers[i].call((uint32_t)fno, cd_buffers[i].in, (uint32_t)size,
                       cd_buffers[i].out, sizeof(cd_buffers[i].out));
    return cd_buffers[i].out;
}
void ob_iop_cd_servers_init(void) {
    unsigned i;
    uint32_t owner = 0;
    for (i = 0; i < CD_SERVERS; ++i) {
        if (cd_servers[i].stack) {
            owner = cd_servers[i].sid;
            ob_iop_rpc_server_start((int)owner, cd_server, cd_buffers[i].in, 0x51, cd_servers[i].stack);
        } else ob_iop_rpc_server_join((int)cd_servers[i].sid, cd_server, cd_buffers[i].in, (int)owner);
    }
}

static const struct server servers[] = {
    {0x80000003u, iop_heap},
    {0x80000006u, load_file},
};
#define SERVER_COUNT (sizeof(servers) / sizeof(servers[0]))
_Static_assert((RPC_AREA_IOP & 0x1fffffu) + SERVER_COUNT * RPC_BUFFER_SIZE <= OB_SYSMEM_HOLE_END,
               "RPC buffers exceed the reserved IOP window");

static uint32_t descriptor(unsigned i) { return (RPC_AREA_IOP & 0x1fffffu) + i * RPC_BUFFER_SIZE; }

/* Reply to BIND/CALL. BIND and CALL with rmode != 0 end with an RPC_END command
   to the EE command buffer; CALL with rmode 0 instead writes the end packet,
   with rec_id/rpc_id cleared, into the client's own packet (pkt_addr), which
   the EE polls (public sifrpc protocol). Interrupts stay off while the shared
   staging buffer is in use. */
void ob_iop_rpc_reply(const struct ob_rpc_request *r, uint32_t server, uint32_t buffer,
                      uint32_t cbuffer, uint32_t data_source, uint32_t data_bytes) {
    struct ob_sif0_segment seg[2];
    struct ob_rpc_request copy = *r;
    uint32_t end_packet[16] __attribute__((aligned(16))); /* Per call: threads reply concurrently. */
    unsigned n = 0;
    int size, command = r->command == OB_SIF_CMD_RPC_BIND || r->rmode;
    if (!command) copy.rec_id = copy.rpc_id = 0;
    size = ob_rpc_build_end(end_packet, sizeof(end_packet), &copy, server, buffer, cbuffer);
    if (size < 0) return;
    if (data_bytes && r->recvbuf) {
        seg[n].iop_source = data_source;
        seg[n].ee_destination = r->recvbuf;
        seg[n].irq = 0;
        seg[n++].qwc = (data_bytes + 15) / 16;
    }
    seg[n].iop_source = (uint32_t)(uintptr_t)end_packet;
    seg[n].irq = command;
    seg[n++].qwc = ((uint32_t)size + 15) / 16;
    if (command) {
        if (ob_iop_sif_ee_buffer(&seg[n - 1].ee_destination)) {
            ob_puts("[openbios:iop] RPC_END: no EE buffer\n");
            return;
        }
    } else seg[n - 1].ee_destination = r->pkt_addr;
    if (ob_iop_sif0_send_chain(seg, n) != OB_OK) ob_puts("[openbios:iop] RPC_END send failed\n");
#ifdef OB_TRACE_RPC
    {
        extern uint32_t ob_iop_trace_t[3];
        ob_puts("[openbios:rpc] end server="); ob_puthex(server); ob_puts(" send=");
        ob_puthex(ob_iop_trace_t[0]); ob_puts(" started="); ob_puthex(ob_iop_trace_t[1]);
        ob_puts(" done="); ob_puthex(ob_iop_trace_t[2]); ob_puts("\n");
    }
#endif
}
static void send_end(const struct ob_rpc_request *r, uint32_t server, uint32_t buffer,
                     uint32_t data_source, uint32_t data_bytes) {
    ob_iop_rpc_reply(r, server, buffer, 0, data_source, data_bytes);
}

void ob_iop_rpc_handle(const struct ob_rpc_request *r) {
    unsigned i;
#ifdef OB_TRACE_RPC
    {
        static unsigned traced;
        if (traced < 200000) {
            ++traced;
            ob_puts("[openbios:rpc] cmd="); ob_puthex(r->command);
            ob_puts(" sid/server="); ob_puthex(r->command == OB_SIF_CMD_RPC_BIND ? r->sid : r->server);
            ob_puts(" fno="); ob_puthex(r->rpc_number); ob_puts(" size="); ob_puthex(r->send_size);
            ob_puts(" rsize="); ob_puthex(r->recv_size); ob_puts(" rmode="); ob_puthex(r->rmode);
            ob_puts(" t="); ob_puthex((uint32_t)(ob_iop_system_time() / 36864u)); ob_puts("\n");
        }
    }
#endif
    if (ob_iop_module_rpc(r)) return; /* Servers registered by IRX modules. */
    for (i = 0; i < SERVER_COUNT; ++i) {
        if (r->command == OB_SIF_CMD_RPC_BIND && servers[i].sid == r->sid) break;
        if (r->command == OB_SIF_CMD_RPC_CALL && descriptor(i) == r->server) break;
    }
    if (r->command == OB_SIF_CMD_RPC_BIND) {
        if (i == SERVER_COUNT) {
            /* Unknown server: a null server pointer makes the client retry. */
            ob_puts("[openbios:iop] RPC bind to unknown sid="); ob_puthex(r->sid); ob_puts("\n");
            send_end(r, 0, 0, 0, 0);
            return;
        }
        send_end(r, descriptor(i), descriptor(i), 0, 0);
        return;
    }
    if (i == SERVER_COUNT) {
        ob_puts("[openbios:iop] RPC call to unknown server="); ob_puthex(r->server);
        ob_puts(" fno="); ob_puthex(r->rpc_number); ob_puts("\n");
        send_end(r, r->server, 0, 0, 0);
        return;
    }
    {
        uint32_t in_size = r->send_size < RPC_BUFFER_SIZE ? r->send_size : RPC_BUFFER_SIZE;
        uint32_t out_size = r->recv_size < REPLY_DATA_MAX ? r->recv_size : REPLY_DATA_MAX;
        uint32_t bytes;
        if (r->recv_size && ((r->recvbuf & 15) || !r->recvbuf)) {
            ob_puts("[openbios:iop] RPC reply buffer not qword aligned\n");
            out_size = 0;
        }
        ob_zero((void *)REPLY_DATA_IOP, REPLY_DATA_MAX);
        bytes = servers[i].call(r->rpc_number, (const uint8_t *)(descriptor(i) | 0xa0000000u),
                                in_size, (uint8_t *)REPLY_DATA_IOP, out_size);
        send_end(r, r->server, 0, REPLY_DATA_IOP, bytes);
    }
}

void ob_iop_rpc_reset(void) {
    /* Modules and IOP heap blocks belong to the previous IOP boot. */
    ob_iop_kernel_reset();
    ob_sysmem_init();
    {   /* Clear freed module memory in ascending order, which keeps translated
           code caches coherent when the next modules load on top of it. */
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(0xa0000000u | ob_sysmem_base());
        volatile uint32_t *end = (volatile uint32_t *)(uintptr_t)(0xa0000000u | OB_SYSMEM_END);
        while (p < end) {
            if (p == (volatile uint32_t *)(uintptr_t)(0xa0000000u | OB_SYSMEM_HOLE_START))
                p = (volatile uint32_t *)(uintptr_t)(0xa0000000u | OB_SYSMEM_HOLE_END);
            *p++ = 0;
        }
    }
    ob_loadcore_init();
    ob_iop_libraries_init();
    ob_iop_kernel_libraries_init();
    ob_iop_dmacman_init();
    ob_iop_cdvdman_init();
    ob_iop_ioman_init();
    ob_iop_modload_init();
    ob_iop_secrman_init();
    ob_iop_timrman_init();
    ob_iop_sifrpc_init();
    ob_iop_fileio_init();
    ob_iop_cd_servers_init();
}
