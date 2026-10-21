/* SPDX-License-Identifier: MIT */
/* ioman: the IOP file layer. Paths are "device<unit>:rest"; each device is a
   driver with an operations table (iop_device_t / iop_device_ops_t layouts)
   registered with AddDrv. Built-in drivers serve cdrom (ISO9660) and rom
   (the ROM directory). */
#include "openbios/iop.h"
#include "openbios/ioman.h"
#include "openbios/disc.h"
#include "openbios/iso9660.h"
#include "openbios/rom.h"
#include "openbios/platform.h"

int ob_iop_register_builtin(struct ob_irx_exports *exports, uint32_t count);

#define DEVICES 16
#define FILES 32
static struct ob_iop_device *devices[DEVICES];
static struct { int used, dir; struct ob_iop_file f; } files[FILES];

static uint32_t lock(void) {
    uint32_t v; __asm__ volatile ("mfc0 %0, $12\n\tnop" : "=r"(v));
    __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(v & ~1u) : "memory");
    return v;
}
static void unlock(uint32_t v) { __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(v) : "memory"); }

static int name_equal(const char *a, const char *b, unsigned n) {
    unsigned i;
    for (i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return b[n] == 0;
}
/* Split "dev<unit>:rest". Leading blanks are skipped; returns the device. */
static struct ob_iop_device *parse(const char *path, int *unit, const char **rest) {
    unsigned n = 0, i;
    int u = 0;
    if (!path) return 0;
    while (*path == ' ' || *path == '\t') ++path;
    while (path[n] && path[n] != ':' && !(path[n] >= '0' && path[n] <= '9')) ++n;
    i = n;
    while (path[i] >= '0' && path[i] <= '9') u = u * 10 + (path[i++] - '0');
    if (path[i] != ':') return 0;
    *unit = u;
    *rest = path + i + 1;
    for (u = 0; u < DEVICES; ++u)
        if (devices[u] && name_equal(path, devices[u]->name, n)) return devices[u];
    return 0;
}
static int alloc_fd(void) {
    uint32_t s = lock();
    int i;
    for (i = 0; i < FILES; ++i) if (!files[i].used) { files[i].used = 1; break; }
    unlock(s);
    return i < FILES ? i : -OB_IO_EMFILE;
}
static struct ob_iop_file *fd_file(int fd, int dir) {
    if (fd < 0 || fd >= FILES || !files[fd].used || files[fd].dir != dir) return 0;
    return &files[fd].f;
}
static int open_common(const char *name, int flags, int dir) {
    const char *rest;
    int unit, fd, r;
    struct ob_iop_device *d = parse(name, &unit, &rest);
    if (!d) return -OB_IO_ENODEV;
    fd = alloc_fd();
    if (fd < 0) return fd;
    files[fd].dir = dir;
    files[fd].f.mode = flags;
    files[fd].f.unit = unit;
    files[fd].f.device = d;
    files[fd].f.privdata = 0;
    r = dir ? d->ops->dopen(&files[fd].f, rest) : d->ops->open(&files[fd].f, rest, flags);
    if (r < 0) { files[fd].used = 0; return r; }
    return fd;
}
int ob_io_open(const char *name, int flags) {
    int r = open_common(name, flags, 0);
#ifdef OB_TRACE_RPC
    ob_puts("[openbios:io] open "); ob_puts(name); ob_puts(" -> "); ob_puthex((uint32_t)r); ob_puts("\n");
#endif
    return r;
}
int ob_io_dopen(const char *name) { return open_common(name, OB_IO_O_DIROPEN, 1); }
static int close_common(int fd, int dir) {
    struct ob_iop_file *f = fd_file(fd, dir);
    int r;
    if (!f) return -OB_IO_EBADF;
    r = dir ? f->device->ops->dclose(f) : f->device->ops->close(f);
    files[fd].used = 0;
    return r;
}
int ob_io_close(int fd) { return close_common(fd, 0); }
int ob_io_dclose(int fd) { return close_common(fd, 1); }
int ob_io_read(int fd, void *ptr, int size) {
    struct ob_iop_file *f = fd_file(fd, 0);
    int r = f ? f->device->ops->read(f, ptr, size) : -OB_IO_EBADF;
    return r;
}
int ob_io_write(int fd, void *ptr, int size) {
    struct ob_iop_file *f = fd_file(fd, 0);
    return f ? f->device->ops->write(f, ptr, size) : -OB_IO_EBADF;
}
int ob_io_lseek(int fd, int offset, int whence) {
    struct ob_iop_file *f = fd_file(fd, 0);
    return f ? f->device->ops->lseek(f, offset, whence) : -OB_IO_EBADF;
}
int ob_io_ioctl(int fd, int cmd, void *param) {
    struct ob_iop_file *f = fd_file(fd, 0);
    return f ? f->device->ops->ioctl(f, cmd, param) : -OB_IO_EBADF;
}
int ob_io_dread(int fd, struct ob_io_dirent *buf) {
    struct ob_iop_file *f = fd_file(fd, 1);
    return f ? f->device->ops->dread(f, buf) : -OB_IO_EBADF;
}
/* Path operations use a transient file object. */
#define PATH_OP(op, call) do { \
    const char *rest; int unit; struct ob_iop_file tmp; \
    struct ob_iop_device *d = parse(name, &unit, &rest); \
    if (!d) return -OB_IO_ENODEV; \
    tmp.mode = 0; tmp.unit = unit; tmp.device = d; tmp.privdata = 0; \
    return d->ops->op call; \
} while (0)
int ob_io_remove(const char *name) { PATH_OP(remove, (&tmp, rest)); }
int ob_io_mkdir(const char *name) { PATH_OP(mkdir, (&tmp, rest)); }
int ob_io_rmdir(const char *name) { PATH_OP(rmdir, (&tmp, rest)); }
int ob_io_getstat(const char *name, struct ob_io_stat *stat) { PATH_OP(getstat, (&tmp, rest, stat)); }
int ob_io_chstat(const char *name, struct ob_io_stat *stat, unsigned mask) {
    PATH_OP(chstat, (&tmp, rest, stat, mask));
}
int ob_io_format(const char *name) { PATH_OP(format, (&tmp)); }

/* Extended (iomanX) operations reach only drivers that declare them. */
static int extended(const struct ob_iop_device *d) { return (d->type & OB_IO_DT_FSEXT) != 0; }
#define EXT_PATH_OP(op, call) do { \
    const char *rest; int unit; struct ob_iop_file tmp; \
    struct ob_iop_device *d = parse(name, &unit, &rest); \
    if (!d) return -OB_IO_ENODEV; \
    if (!extended(d) || !d->ops->op) return -OB_IO_EUNSUP; \
    tmp.mode = 0; tmp.unit = unit; tmp.device = d; tmp.privdata = 0; \
    return d->ops->op call; \
} while (0)
static int io_rename(const char *name, const char *new_name) {
    const char *new_rest;
    int new_unit;
    struct ob_iop_device *nd = parse(new_name, &new_unit, &new_rest);
    /* The new name may omit the device; a different device cannot be targeted. */
    if (nd && nd != parse(name, &new_unit, &new_rest)) return -OB_IO_EXDEV;
    if (!nd) new_rest = new_name;
    EXT_PATH_OP(rename, (&tmp, rest, new_rest));
}
static int io_chdir(const char *name) { EXT_PATH_OP(chdir, (&tmp, rest)); }
static int io_sync(const char *name, int flag) { EXT_PATH_OP(sync, (&tmp, rest, flag)); }
static int io_mount(const char *fsname, const char *name, int flag, void *arg, int arglen) {
    EXT_PATH_OP(mount, (&tmp, fsname, rest, flag, arg, arglen));
}
static int io_umount(const char *name) { EXT_PATH_OP(umount, (&tmp, rest)); }
int ob_io_devctl(const char *name, int cmd, void *arg, unsigned arglen, void *buf, unsigned buflen) {
    EXT_PATH_OP(devctl, (&tmp, rest, cmd, arg, arglen, buf, buflen));
}
static int io_symlink(const char *name, const char *new_name) {
    EXT_PATH_OP(symlink, (&tmp, rest, new_name));
}
static int io_readlink(const char *name, char *buf, unsigned buflen) {
    EXT_PATH_OP(readlink, (&tmp, rest, buf, buflen));
}
static int64_t io_lseek64(int fd, int64_t offset, int whence) {
    struct ob_iop_file *f = fd_file(fd, 0);
    if (!f) return -OB_IO_EBADF;
    if (!extended(f->device) || !f->device->ops->lseek64) return -OB_IO_EUNSUP;
    return f->device->ops->lseek64(f, offset, whence);
}
static int io_ioctl2(int fd, int cmd, void *arg, unsigned arglen, void *buf, unsigned buflen) {
    struct ob_iop_file *f = fd_file(fd, 0);
    if (!f) return -OB_IO_EBADF;
    if (!extended(f->device) || !f->device->ops->ioctl2) return -OB_IO_EUNSUP;
    return f->device->ops->ioctl2(f, cmd, arg, arglen, buf, buflen);
}
static int io_get_dev_type(int fd) {
    if (fd < 0 || fd >= FILES || !files[fd].used) return -OB_IO_EBADF;
    return (int)files[fd].f.device->type;
}

int ob_io_add_driver(struct ob_iop_device *d) {
    unsigned i, free_slot = DEVICES, n;
    if (!d || !d->name || !d->ops) return -1;
    for (n = 0; d->name[n]; ++n) {}
    for (i = 0; i < DEVICES; ++i) {
        if (devices[i] && name_equal(d->name, devices[i]->name, n)) return -1;
        if (!devices[i] && free_slot == DEVICES) free_slot = i;
    }
    if (free_slot == DEVICES) return -1;
    if (d->ops->init && d->ops->init(d) < 0) return -1;
    devices[free_slot] = d;
    return 0;
}
int ob_io_del_driver(const char *name) {
    unsigned i, n;
    if (!name) return -1;
    for (n = 0; name[n]; ++n) {}
    for (i = 0; i < DEVICES; ++i)
        if (devices[i] && name_equal(name, devices[i]->name, n)) {
            if (devices[i]->ops->deinit) devices[i]->ops->deinit(devices[i]);
            devices[i] = 0;
            return 0;
        }
    return -1;
}

/* ---- built-in drivers ---- */
static int dev_nop(void) { return 0; }
static int dev_unsupported(void) { return -OB_IO_EIO; }
static int dev_readonly(void) { return -OB_IO_EROFS; }

/* cdrom: read-only ISO9660 files and directories on the inserted disc. */
struct cd_handle { int used; uint32_t lba, size, pos; };
static struct cd_handle cd_handles[FILES];
static uint8_t *cd_bounce;
static struct cd_handle *cd_alloc(void) {
    unsigned i;
    for (i = 0; i < FILES; ++i) if (!cd_handles[i].used) { cd_handles[i].used = 1; return &cd_handles[i]; }
    return 0;
}
static void stat_from_entry(const struct ob_iso_entry *e, struct ob_io_stat *st) {
    unsigned year = 1900u + e->date[0];
    ob_zero(st, sizeof(*st));
    st->mode = (e->flags & 2) ? OB_IO_S_DIR : OB_IO_S_FILE;
    st->size = e->size;
    /* ioman time: unused, second, minute, hour, day, month, year (16-bit). */
    st->mtime[1] = e->date[5]; st->mtime[2] = e->date[4]; st->mtime[3] = e->date[3];
    st->mtime[4] = e->date[2]; st->mtime[5] = e->date[1];
    st->mtime[6] = (uint8_t)year; st->mtime[7] = (uint8_t)(year >> 8);
    ob_copy(st->ctime, st->mtime, 8);
    ob_copy(st->atime, st->mtime, 8);
}
/* Mounting and walking directories keeps a 2 KiB sector on the stack, more
   than a lot of module threads have (MH's EzMIDI thread overflowed into its
   neighbour opening EFF.BD). Do it on cdvdman's lookup stack. */
struct cd_lookup_args { const char *name; struct ob_iso_entry *e; };
static int cd_lookup_thunk(void *p) {
    struct cd_lookup_args *a = p;
    const struct ob_iso *iso = ob_iop_cd_iso();
    return !iso || ob_iso_lookup(iso, a->name, a->e);
}
static int cd_lookup(const char *name, struct ob_iso_entry *e) {
    struct cd_lookup_args a = {name, e};
    return ob_iop_cd_on_stack(cd_lookup_thunk, &a);
}
static int cd_open(struct ob_iop_file *f, const char *name, int flags) {
    struct ob_iso_entry e;
    struct cd_handle *h;
    /* The mode is ignored: callers pass cdvdman read-speed hints in it
       (the low half and bit 30). */
    (void)flags;
    if (cd_lookup(name, &e)) return -OB_IO_ENOENT;
    if (e.flags & 2) return -OB_IO_EISDIR;
    if (!(h = cd_alloc())) return -OB_IO_EMFILE;
    h->lba = e.lba; h->size = e.size; h->pos = 0;
    f->privdata = h;
    return 0;
}
static int cd_dopen(struct ob_iop_file *f, const char *name) {
    struct ob_iso_entry e;
    struct cd_handle *h;
    if (cd_lookup(name, &e)) return -OB_IO_ENOENT;
    if (!(e.flags & 2)) return -OB_IO_ENOTDIR;
    if (!(h = cd_alloc())) return -OB_IO_EMFILE;
    h->lba = e.lba; h->size = e.size; h->pos = 0;
    f->privdata = h;
    return 0;
}
static int cd_close(struct ob_iop_file *f) {
    struct cd_handle *h = f->privdata;
    if (h) h->used = 0;
    return 0;
}
static int cd_read(struct ob_iop_file *f, void *ptr, int size) {
    struct cd_handle *h = f->privdata;
    uint8_t *out = ptr;
    uint32_t left, done = 0;
    if (size < 0) return -OB_IO_EINVAL;
    left = h->size - h->pos;
    if ((uint32_t)size < left) left = (uint32_t)size;
    if (!cd_bounce && !(cd_bounce = ob_sysmem_alloc(OB_ALLOC_LAST, 2048, 0))) return -OB_IO_ENOMEM;
    while (left) {
        uint32_t sector = h->pos / 2048, within = h->pos % 2048, n;
        if (!within && left >= 2048 && !((uintptr_t)(out + done) & 3)) {
            /* Whole sectors straight into the caller's buffer. */
            n = left / 2048;
            if (ob_iop_cd_read_sectors(h->lba + sector, n, out + done)) return -OB_IO_EIO;
            n *= 2048;
        } else {
            n = 2048 - within;
            if (n > left) n = left;
            if (ob_iop_cd_read_sectors(h->lba + sector, 1, cd_bounce)) return -OB_IO_EIO;
            ob_copy(out + done, cd_bounce + within, n);
        }
        done += n; left -= n; h->pos += n;
    }
    return (int)done;
}
static int cd_lseek(struct ob_iop_file *f, int offset, int whence) {
    struct cd_handle *h = f->privdata;
    int64_t base = whence == OB_IO_SEEK_SET ? 0 : whence == OB_IO_SEEK_CUR ? (int64_t)h->pos :
                   whence == OB_IO_SEEK_END ? (int64_t)h->size : -1;
    if (base < 0 || base + offset < 0 || base + offset > h->size) return -OB_IO_EINVAL;
    h->pos = (uint32_t)(base + offset);
    return (int)h->pos;
}
struct cd_dread_args { struct cd_handle *h; struct ob_iso_entry *e; };
static int cd_dread_thunk(void *p) {
    struct cd_dread_args *a = p;
    const struct ob_iso *iso = ob_iop_cd_iso();
    return !iso || ob_iso_dir_next(iso, a->h->lba, a->h->size, &a->h->pos, a->e);
}
static int cd_dread(struct ob_iop_file *f, struct ob_io_dirent *d) {
    struct cd_handle *h = f->privdata;
    struct ob_iso_entry e;
    struct cd_dread_args a = {h, &e};
    unsigned i;
    if (ob_iop_cd_on_stack(cd_dread_thunk, &a)) return 0;
    stat_from_entry(&e, &d->stat);
    for (i = 0; e.name[i] && i < sizeof(d->name) - 1; ++i) d->name[i] = e.name[i];
    d->name[i] = 0;
    d->unknown = 0;
    return (int)i;
}
static int cd_getstat(struct ob_iop_file *f, const char *name, struct ob_io_stat *st) {
    struct ob_iso_entry e;
    (void)f;
    if (cd_lookup(name, &e)) return -OB_IO_ENOENT;
    stat_from_entry(&e, st);
    return 0;
}
#define OP(fn) ((void *)(fn))
static struct ob_iop_device_ops cd_ops = {
    OP(dev_nop), OP(dev_nop), OP(dev_readonly), OP(cd_open), OP(cd_close), OP(cd_read),
    OP(dev_readonly), OP(cd_lseek), OP(dev_unsupported), OP(dev_readonly), OP(dev_readonly),
    OP(dev_readonly), OP(cd_dopen), OP(cd_close), OP(cd_dread), OP(cd_getstat), OP(dev_readonly),
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0 /* not an extended driver */
};
static struct ob_iop_device cd_device = {"cdrom", OB_IO_DT_FS, 1, "ISO9660 CD/DVD", &cd_ops};

/* rom: files in this firmware's ROM directory. */
struct rom_handle { int used; const uint8_t *data; uint32_t size, pos; };
static struct rom_handle rom_handles[8];
static int rom_open(struct ob_iop_file *f, const char *name, int flags) {
    struct ob_rom_file file;
    char base[12];
    unsigned i;
    if ((flags & 3) != OB_IO_O_RDONLY) return -OB_IO_EROFS;
    while (*name == '/' || *name == '\\') ++name;
    for (i = 0; i < sizeof(base) - 1 && name[i] && name[i] != ';'; ++i) base[i] = name[i];
    base[i] = 0;
    if (ob_rom_find((const void *)0xbfc00000u, OB_ROM_SIZE, base, &file)) return -OB_IO_ENOENT;
    for (i = 0; i < 8 && rom_handles[i].used; ++i) {}
    if (i == 8) return -OB_IO_EMFILE;
    rom_handles[i].used = 1;
    rom_handles[i].data = file.data;
    rom_handles[i].size = file.size;
    rom_handles[i].pos = 0;
    f->privdata = &rom_handles[i];
    return 0;
}
static int rom_close(struct ob_iop_file *f) {
    struct rom_handle *h = f->privdata;
    if (h) h->used = 0;
    return 0;
}
static int rom_read(struct ob_iop_file *f, void *ptr, int size) {
    struct rom_handle *h = f->privdata;
    uint32_t n = h->size - h->pos;
    if (size < 0) return -OB_IO_EINVAL;
    if ((uint32_t)size < n) n = (uint32_t)size;
    ob_copy(ptr, h->data + h->pos, n);
    h->pos += n;
    return (int)n;
}
static int rom_lseek(struct ob_iop_file *f, int offset, int whence) {
    struct rom_handle *h = f->privdata;
    int64_t base = whence == OB_IO_SEEK_SET ? 0 : whence == OB_IO_SEEK_CUR ? (int64_t)h->pos :
                   whence == OB_IO_SEEK_END ? (int64_t)h->size : -1;
    if (base < 0 || base + offset < 0 || base + offset > h->size) return -OB_IO_EINVAL;
    h->pos = (uint32_t)(base + offset);
    return (int)h->pos;
}
static struct ob_iop_device_ops rom_ops = {
    OP(dev_nop), OP(dev_nop), OP(dev_readonly), OP(rom_open), OP(rom_close), OP(rom_read),
    OP(dev_readonly), OP(rom_lseek), OP(dev_unsupported), OP(dev_readonly), OP(dev_readonly),
    OP(dev_readonly), OP(dev_unsupported), OP(dev_unsupported), OP(dev_unsupported),
    OP(dev_unsupported), OP(dev_readonly),
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0 /* not an extended driver */
};
static struct ob_iop_device rom_device = {"rom", OB_IO_DT_FS, 1, "ROM directory", &rom_ops};

/* ---- ioman exports ---- */
static struct { uint32_t magic, next; uint16_t version, mode; char name[8] __attribute__((nonstring));
                uint32_t f[36]; } ioman = {OB_IRX_EXPORT_MAGIC, 0, 0x0104, 0, "ioman", {0}};
#define FN(p) ((uint32_t)(uintptr_t)(p))
void ob_iop_ioman_init(void) {
    ob_zero(devices, sizeof(devices));
    ob_zero(files, sizeof(files));
    ob_zero(cd_handles, sizeof(cd_handles));
    ob_zero(rom_handles, sizeof(rom_handles));
    cd_bounce = 0;
    ob_io_add_driver(&rom_device);
    ob_io_add_driver(&cd_device);
    ioman.f[0] = ioman.f[1] = ioman.f[2] = ioman.f[3] = FN(dev_nop);
    ioman.f[4] = FN(ob_io_open);    ioman.f[5] = FN(ob_io_close);
    ioman.f[6] = FN(ob_io_read);    ioman.f[7] = FN(ob_io_write);
    ioman.f[8] = FN(ob_io_lseek);   ioman.f[9] = FN(ob_io_ioctl);
    ioman.f[10] = FN(ob_io_remove); ioman.f[11] = FN(ob_io_mkdir);
    ioman.f[12] = FN(ob_io_rmdir);  ioman.f[13] = FN(ob_io_dopen);
    ioman.f[14] = FN(ob_io_dclose); ioman.f[15] = FN(ob_io_dread);
    ioman.f[16] = FN(ob_io_getstat); ioman.f[17] = FN(ob_io_chstat);
    ioman.f[18] = FN(ob_io_format);
    ioman.f[20] = FN(ob_io_add_driver); ioman.f[21] = FN(ob_io_del_driver);
    ioman.f[19] = ioman.f[23] = ioman.f[24] = FN(dev_nop);
    ioman.f[22] = FN(io_get_dev_type);
    ioman.f[25] = FN(io_rename);    ioman.f[26] = FN(io_chdir);
    ioman.f[27] = FN(io_sync);      ioman.f[28] = FN(io_mount);
    ioman.f[29] = FN(io_umount);    ioman.f[30] = FN(io_lseek64);
    ioman.f[31] = FN(ob_io_devctl);    ioman.f[32] = FN(io_symlink);
    ioman.f[33] = FN(io_readlink);  ioman.f[34] = FN(io_ioctl2);
    ob_iop_register_builtin((void *)&ioman, 35);
}
