/* SPDX-License-Identifier: MIT */
#include "openbios/disc.h"
#include "openbios/sif.h"
#include "openbios/iso9660.h"
#include "openbios/iop.h"
#include "openbios/ioman.h"

#define REG8(addr) (*(volatile uint8_t *)(uintptr_t)(addr))
#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define CDVD 0xbf402000u
#define DMA3 0xbf8010b0u

/* Poll budget before a wait starts blocking in 1 ms steps, and the step limit. */
#define SPIN_POLLS 64u
#define WAIT_STEPS 10000u
static int cdvd_ready(void) { return (REG8(CDVD + 5) & 0x40) != 0; }
static int dma3_idle(void) { return !(REG32(DMA3 + 8) & 0x01000000); }
static int cdvd_error(void) { return (REG8(CDVD + 5) & 1) != 0; }
/* Wait for done(), giving the CPU to other threads once a short spin has not
   sufficed. failed() aborts the wait early. 0 when done, -1 on timeout. */
static int wait_for(int (*done)(void), int (*failed)(void)) {
    unsigned i, steps = 0;
    for (i = 0;; ++i) {
        if (failed && failed()) return 1;
        if (done()) return 0;
        if (i < SPIN_POLLS) continue;
        if (!ob_kernel_pause(1000)) { if (i > 200000000u) return -1; continue; }
        if (++steps > WAIT_STEPS) return -1;
    }
}
static void dma3_start(uint32_t dest, uint32_t sectors) {
    REG32(DMA3 + 8) = 0;
    REG32(DMA3) = dest & 0x1fffffu;
    REG32(DMA3 + 4) = (sectors << 16) | 0x200; /* blocks of 512 words */
    REG32(DMA3 + 8) = 0x41000200;
}
/* Read count 2048-byte user sectors starting at lba with one N-command, since
   each command costs a seek. DMA3 fills buf[0] and buf[1] alternately in
   chunk-sector pieces; sink() consumes one while the drive fills the other. */
/* Several threads issue drive commands; only one runs at a time. */
static volatile int drive_busy;
static uint32_t irq_off(void) {
    uint32_t s; __asm__ volatile ("mfc0 %0, $12\n\tnop" : "=r"(s));
    __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s & ~1u) : "memory");
    return s;
}
static void irq_restore(uint32_t s) { __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s) : "memory"); }
static void drive_acquire(void) {
    for (;;) {
        uint32_t s = irq_off();
        if (!drive_busy) { drive_busy = 1; irq_restore(s); return; }
        irq_restore(s);
        ob_kernel_pause(200);
    }
}
static int read_stream_locked(uint32_t lba, uint32_t count, uint8_t *const buf[2], uint32_t chunk,
                              int (*sink)(void *ctx, const uint8_t *data, uint32_t first, uint32_t n),
                              void *ctx, volatile uint32_t *detail);
/* Disc ready for a new command. A drive that is busy with one of our own reads
   still counts: the next read waits its turn. Secret Agent Clank checks this
   before each music stream read, while the EE's file reads keep the drive busy,
   and stops streaming at the first "not ready". */
int ob_iop_cd_disc_ready(void) {
    uint8_t status = REG8(CDVD + 5);
    return (status & 0xc0) == 0x40 || ((status & 0x80) && drive_busy);
}
/* The disc type register is sampled on every kernel tick and at each use; any
   change starts a new media generation, invalidating all cached disc state. */
static volatile uint32_t media_generation = 1;
static volatile int sampled_type = -1;
static uint32_t media(void) {
    int type = REG8(CDVD + 0x0f);
    if (type != sampled_type) { sampled_type = type; ++media_generation; }
    return media_generation;
}

/* Dual-layer DVD addressing. Disc LSNs count layer 1's 16-sector lead-in,
   which the drive does not return, so reads that start in layer 1 move back
   16 sectors. The layer start comes from the TOC, read once per disc. */
static uint32_t layer_generation;
static int layer_dual;
static uint32_t layer1_lsn;
int ob_iop_cd_layer_info(int *dual, uint32_t *layer1) {
    static uint8_t toc[2064] __attribute__((aligned(4)));
    int type = REG8(CDVD + 0x0f);
    if (media() != layer_generation) {
        layer_dual = 0;
        layer1_lsn = 0;
        if ((type == 0x14 || type == 0xfd || type == 0xfe) && ob_iop_cd_read_toc(toc) > 0 && (toc[14] & 0x60)) {
            layer_dual = 1;
            layer1_lsn = ((uint32_t)toc[21] << 16 | (uint32_t)toc[22] << 8 | toc[23]) - 0x30000u + 1u;
        }
        layer_generation = media();
    }
    if (dual) *dual = layer_dual;
    if (layer1) *layer1 = layer1_lsn;
    return 1;
}
/* IOP reboot: the threads that may have been reading died mid-command. Stop
   their DMA before its buffers are reused, let the drive finish, and drop the
   lock they held. */
void ob_iop_disc_reboot(void) {
    unsigned i;
    REG32(DMA3 + 8) = 0;
    for (i = 0; i < 2000000u && !cdvd_ready(); ++i) {}
    REG8(CDVD + 8) = 0xff;
    drive_busy = 0;
}
int ob_iop_cd_read_stream(uint32_t lba, uint32_t count, uint8_t *const buf[2], uint32_t chunk,
                          int (*sink)(void *ctx, const uint8_t *data, uint32_t first, uint32_t n),
                          void *ctx, volatile uint32_t *detail) {
    int r, dual;
    uint32_t layer1;
    ob_iop_cd_layer_info(&dual, &layer1);
    if (dual && lba >= layer1) lba -= 16;
    drive_acquire();
    r = read_stream_locked(lba, count, buf, chunk, sink, ctx, detail);
    drive_busy = 0;
    return r;
}
static int read_stream_locked(uint32_t lba, uint32_t count, uint8_t *const buf[2], uint32_t chunk,
                              int (*sink)(void *ctx, const uint8_t *data, uint32_t first, uint32_t n),
                              void *ctx, volatile uint32_t *detail) {
    unsigned i;
    uint8_t params[11];
    uint32_t done = 0, n;
    int r, piece = 0;
    *detail = 1;
    if (!count || count > 0xffff || !chunk || ((uintptr_t)buf[0] & 3) || ((uintptr_t)buf[1] & 3))
        return OB_EINVAL;
    if (wait_for(cdvd_ready, 0)) return OB_EBUSY;
    REG8(CDVD + 8) = 0xff; /* Acknowledge completion/error interrupt. */
    REG8(CDVD + 0x3a) = 0; /* Read plain user sectors. */
    REG32(0xbf8010f0u) |= 0x8000; /* DMA channel 3 enable. */
    n = count < chunk ? count : chunk;
    dma3_start((uint32_t)(uintptr_t)buf[0], n);
    for (i = 0; i < 4; ++i) params[i] = (uint8_t)(lba >> (8 * i));
    for (i = 0; i < 4; ++i) params[4 + i] = (uint8_t)(count >> (8 * i));
    /* Retries, spindle control (DVD: 0x83 CAV 4x; CD: 0x85 CAV max, unconfirmed),
       2048-byte sectors. */
    params[8] = 16;
    params[9] = (REG8(CDVD + 0x0f) >= 0x14) ? 0x83 : 0x85;
    params[10] = 0;
    for (i = 0; i < sizeof(params); ++i) REG8(CDVD + 5) = params[i];
    REG8(CDVD + 4) = 6; /* Normal read supports CD and DVD user sectors. */
    *detail = 2;
    for (;;) {
        uint32_t next;
        r = wait_for(dma3_idle, cdvd_error);
        if (r > 0) {
            *detail = 0x100u | REG8(CDVD + 6);
            REG32(DMA3 + 8) = 0;
            REG8(CDVD + 8) = 0xff;
            return OB_EFORMAT;
        }
        if (r < 0) {
            REG32(DMA3 + 8) = 0;
            return OB_EBUSY;
        }
        next = count - done - n;
        if (next) dma3_start((uint32_t)(uintptr_t)buf[piece ^ 1], next < chunk ? next : chunk);
        if (sink && sink(ctx, buf[piece], done, n)) {
            /* The consumer gave up: let the drive finish into a stopped DMA. */
            REG32(DMA3 + 8) = 0;
            wait_for(cdvd_ready, 0);
            REG8(CDVD + 8) = 0xff;
            return OB_EFORMAT;
        }
        done += n;
        if (!next) break;
        n = next < chunk ? next : chunk;
        piece ^= 1;
    }
    *detail = 3;
    r = wait_for(cdvd_ready, 0);
    REG8(CDVD + 8) = 0xff;
    return r ? OB_EBUSY : OB_OK;
}
/* A drive N-command without data transfer (nop 0, standby 2, stop 3, pause 4,
   seek 5). Returns the drive error register (0 on success) or OB_EBUSY. */
int ob_iop_cd_ncommand(uint8_t cmd, const uint8_t *params, unsigned count) {
    unsigned i;
    int r;
    drive_acquire();
    if (wait_for(cdvd_ready, 0)) { drive_busy = 0; return OB_EBUSY; }
    REG8(CDVD + 8) = 0xff;
    for (i = 0; i < count; ++i) REG8(CDVD + 5) = params[i];
    REG8(CDVD + 4) = cmd;
    r = wait_for(cdvd_ready, 0) ? OB_EBUSY : REG8(CDVD + 6);
    REG8(CDVD + 8) = 0xff;
    drive_busy = 0;
    return r;
}
/* Disc TOC (N-command 9) into toc: 2064 bytes for DVD media, 1024 for CD.
   Returns the byte count or a negative error. */
int ob_iop_cd_read_toc(uint8_t *toc) {
    uint8_t type = REG8(CDVD + 0x0f), param = 0;
    int dvd = type == 0x14 || type == 0xfe || type == 0xfd, r;
    uint32_t words = dvd ? 4 : 32, blocks = dvd ? 129 : 8;
    if (((uintptr_t)toc & 3) || !(dvd || (type >= 0x10 && type <= 0x13) || type == 0xfd ||
                                     type == 0x01 || type == 0x02)) return OB_EINVAL;
    drive_acquire();
    if (wait_for(cdvd_ready, 0)) { drive_busy = 0; return OB_EBUSY; }
    REG8(CDVD + 8) = 0xff;
    REG8(CDVD + 6) = dvd ? 0x84 : 0x80; /* transfer mode for TOC data */
    REG32(0xbf8010f0u) |= 0x8000;
    REG32(DMA3 + 8) = 0;
    REG32(DMA3) = (uint32_t)(uintptr_t)toc & 0x1fffffu;
    REG32(DMA3 + 4) = (blocks << 16) | words;
    REG32(DMA3 + 8) = 0x41000200;
    REG8(CDVD + 5) = param;
    REG8(CDVD + 4) = 9;
    r = wait_for(dma3_idle, cdvd_error);
    if (r) REG32(DMA3 + 8) = 0;
    if (wait_for(cdvd_ready, 0)) r = -1;
    REG8(CDVD + 8) = 0xff;
    drive_busy = 0;
    return r ? OB_EFORMAT : (int)(words * blocks * 4);
}
static int read_sectors(uint32_t lba, uint32_t count, uint32_t dest, volatile uint32_t *detail) {
    uint8_t *const bufs[2] = {(uint8_t *)(uintptr_t)dest, (uint8_t *)(uintptr_t)dest};
    if (!count || count > 0xffff || (dest & 3)) return OB_EINVAL;
    return ob_iop_cd_read_stream(lba, count, bufs, count, 0, 0, detail);
}
static int read_sector(uint32_t lba, volatile uint32_t *detail) {
    return read_sectors(lba, 1, OB_DISC_BUFFER_IOP, detail);
}
int ob_iop_cd_read_sectors(uint32_t lba, uint32_t count, void *dest) {
    volatile uint32_t detail;
    return read_sectors(lba, count, (uint32_t)(uintptr_t)dest, &detail);
}
/* Small cache of filesystem sectors (volume descriptors and directories), since
   every drive command costs a seek. Cleared when the disc changes. */
#define FS_CACHE 4
static struct { uint32_t lba; uint32_t age; } fs_tag[FS_CACHE];
static uint8_t fs_data[FS_CACHE][OB_ISO_SECTOR_SIZE] __attribute__((aligned(4)));
static uint32_t fs_clock;
static uint32_t fs_generation;
/* ctx, when set, points to an LSN added to every sector (the layer-1 volume). */
static int iso_sector(void *ctx, uint32_t lba, uint8_t out[OB_ISO_SECTOR_SIZE]) {
    uint32_t detail;
    int result;
    unsigned i, victim = 0;
    if (ctx) lba += *(const uint32_t *)ctx;
    if (media() != fs_generation) {
        fs_generation = media();
        for (i = 0; i < FS_CACHE; ++i) fs_tag[i].age = 0;
    }
    for (i = 0; i < FS_CACHE; ++i) {
        if (fs_tag[i].age && fs_tag[i].lba == lba) {
            fs_tag[i].age = ++fs_clock;
            ob_copy(out, fs_data[i], OB_ISO_SECTOR_SIZE);
            return OB_OK;
        }
        if (fs_tag[i].age < fs_tag[victim].age) victim = i;
    }
    result = read_sector(lba, (volatile uint32_t *)&detail);
    if (!result) {
        ob_copy(out, (const void *)OB_DISC_BUFFER_IOP, OB_ISO_SECTOR_SIZE);
        ob_copy(fs_data[victim], out, OB_ISO_SECTOR_SIZE);
        fs_tag[victim].lba = lba;
        fs_tag[victim].age = ++fs_clock;
    }
    return result;
}
/* The disc's ISO9660 volume, mounted on first use. */
static struct ob_iso cd_volume;
static int cd_volume_ok;
static uint32_t cd_volume_generation;
const struct ob_iso *ob_iop_cd_iso(void) {
    if (cd_volume_generation != media()) { cd_volume_ok = 0; cd_volume_generation = media(); }
    if (!cd_volume_ok) cd_volume_ok = !ob_iso_mount(&cd_volume, iso_sector, 0, UINT32_MAX);
    return cd_volume_ok ? &cd_volume : 0;
}
/* The ISO9660 volume of a dual-layer DVD's second layer (its descriptors start
   16 sectors into the layer), or 0. Remounted when the disc type changes. */
static struct ob_iso layer1_volume;
static uint32_t layer1_base;
static uint32_t layer1_generation;
static int layer1_ok;
const struct ob_iso *ob_iop_cd_iso_layer1(uint32_t *base) {
    int dual;
    if (media() != layer1_generation) {
        layer1_generation = media();
        layer1_ok = 0;
        if (ob_iop_cd_layer_info(&dual, &layer1_base) && dual)
            layer1_ok = !ob_iso_mount(&layer1_volume, iso_sector, &layer1_base, UINT32_MAX);
    }
    if (base) *base = layer1_base;
    return layer1_ok ? &layer1_volume : 0;
}
/* Look up a cdrom0: path; lba/size of the file. */
int ob_iop_cd_find(const char *path, uint32_t *lba, uint32_t *size) {
    struct ob_iso_file file;
    const struct ob_iso *iso = ob_iop_cd_iso();
    int result;
    if (!iso) return OB_EFORMAT;
    result = ob_iso_find(iso, path, &file);
    if (result) return result;
    *lba = file.lba;
    *size = file.size;
    return OB_OK;
}
/* Read a whole cdrom0: file into a new sysmem block (sector-rounded). */
int ob_iop_cd_load_file(const char *path, void **data, uint32_t *size) {
    uint32_t lba, bytes, sectors;
    void *block;
    int result = ob_iop_cd_find(path, &lba, &bytes);
    if (result) return result;
    sectors = (bytes + 2047) / 2048;
    block = ob_sysmem_alloc(OB_ALLOC_LAST, sectors * 2048 + 16, 0);
    if (!block) return OB_ENOSPC;
    if (sectors && ob_iop_cd_read_sectors(lba, sectors, block)) { ob_sysmem_free(block); return OB_EFORMAT; }
    *data = block;
    *size = bytes;
    return OB_OK;
}

/* The boot mailbox has no interrupt: while the EE has posted work, the kernel
   tick wakes the service thread. */
void ob_iop_disc_tick(void) {
    volatile struct ob_disc_mailbox *m = (void *)OB_DISC_MAILBOX_IOP;
    media();
    if (m->magic == OB_DISC_MAGIC && (m->state & 1)) ob_kernel_wakeup(1);
}
void ob_iop_disc_service(void) {
    volatile struct ob_disc_mailbox *m = (void *)OB_DISC_MAILBOX_IOP;
    int file_fd = -1;
    m->state = 0;
    m->result = 0;
    m->detail = 0;
    m->magic = OB_DISC_MAGIC;
    for (;;) {
        /* SIF1 command receipt wakes this thread (ob_iop_sif_init). */
        if (!ob_iop_sif_poll()) {
            if (m->state == 3) {
                m->result = ob_iop_sif0_send(OB_DISC_BUFFER_IOP, m->lba, m->detail);
                __asm__ volatile ("nop" ::: "memory");
                m->state = 4;
                continue;
            }
            if (m->state == 5) {
                m->detail = (uint32_t)REG8(CDVD + 0x0f) | (uint32_t)REG8(CDVD + 0x0a) << 8;
                m->result = 0;
                __asm__ volatile ("nop" ::: "memory");
                m->state = 6;
                continue;
            }
            if (m->state == 7) {
                if (file_fd >= 0) ob_io_close(file_fd);
                file_fd = ob_io_open((const char *)OB_DISC_BUFFER_IOP, OB_IO_O_RDONLY);
                m->result = file_fd < 0 ? file_fd : ob_io_lseek(file_fd, 0, OB_IO_SEEK_END);
                __asm__ volatile ("nop" ::: "memory");
                m->state = 8;
                continue;
            }
            if (m->state == 9) {
                uint32_t n = m->detail > OB_DISC_BULK_SECTORS * 2048u ? OB_DISC_BULK_SECTORS * 2048u : m->detail;
                int r = ob_io_lseek(file_fd, (int)m->lba, OB_IO_SEEK_SET);
                m->result = r < 0 ? r : ob_io_read(file_fd, (void *)OB_DISC_BULK_IOP, (int)n);
                __asm__ volatile ("nop" ::: "memory");
                m->state = 10;
                continue;
            }
            if (m->state == 11) {
                m->result = file_fd < 0 ? 0 : ob_io_close(file_fd);
                file_fd = -1;
                __asm__ volatile ("nop" ::: "memory");
                m->state = 12;
                continue;
            }
            if (m->state == 1) {
                uint32_t count = m->detail;
                if (count > 1 && count <= OB_DISC_BULK_SECTORS)
                    m->result = read_sectors(m->lba, count, OB_DISC_BULK_IOP, &m->detail);
                else m->result = read_sector(m->lba, &m->detail);
                __asm__ volatile ("nop" ::: "memory");
                m->state = 2;
                continue;
            }
            ob_kernel_sleep();
        }
    }
}
