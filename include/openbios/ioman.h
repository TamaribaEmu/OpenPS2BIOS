/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_IOMAN_H
#define OPENBIOS_IOMAN_H
/* IOP ioman: device drivers and file descriptors. Struct layouts follow the
   public ioman interface (iop_file_t, iop_device_t, iop_device_ops_t,
   io_stat_t, io_dirent_t). */
#include "base.h"

#define OB_IO_O_RDONLY 0x0001
#define OB_IO_O_WRONLY 0x0002
#define OB_IO_O_RDWR 0x0003
#define OB_IO_O_DIROPEN 0x0008
#define OB_IO_SEEK_SET 0
#define OB_IO_SEEK_CUR 1
#define OB_IO_SEEK_END 2
#define OB_IO_DT_FS 0x10
/* io_stat mode: regular file / directory, readable and executable by all. */
#define OB_IO_S_FILE 0x0015u
#define OB_IO_S_DIR 0x0025u
/* Errors are returned negated. */
#define OB_IO_ENOENT 2
#define OB_IO_EIO 5
#define OB_IO_EBADF 9
#define OB_IO_ENOMEM 12
#define OB_IO_ENODEV 19
#define OB_IO_ENOTDIR 20
#define OB_IO_EISDIR 21
#define OB_IO_EINVAL 22
#define OB_IO_EMFILE 24
#define OB_IO_EROFS 30
#define OB_IO_EXDEV 18
#define OB_IO_EUNSUP 48
/* Device type flag: the driver's ops table carries the extended entries. */
#define OB_IO_DT_FSEXT 0x10000000u

struct ob_io_stat {
    uint32_t mode, attr, size;
    uint8_t ctime[8], atime[8], mtime[8];
    uint32_t hisize;
};
struct ob_io_dirent {
    struct ob_io_stat stat;
    char name[256];
    void *unknown;
};
struct ob_iop_device;
struct ob_iop_file {
    int mode;
    int unit;
    struct ob_iop_device *device;
    void *privdata;
};
struct ob_iop_device_ops {
    int (*init)(struct ob_iop_device *d);
    int (*deinit)(struct ob_iop_device *d);
    int (*format)(struct ob_iop_file *f);
    int (*open)(struct ob_iop_file *f, const char *name, int flags);
    int (*close)(struct ob_iop_file *f);
    int (*read)(struct ob_iop_file *f, void *ptr, int size);
    int (*write)(struct ob_iop_file *f, void *ptr, int size);
    int (*lseek)(struct ob_iop_file *f, int offset, int whence);
    int (*ioctl)(struct ob_iop_file *f, int cmd, void *param);
    int (*remove)(struct ob_iop_file *f, const char *name);
    int (*mkdir)(struct ob_iop_file *f, const char *path);
    int (*rmdir)(struct ob_iop_file *f, const char *path);
    int (*dopen)(struct ob_iop_file *f, const char *path);
    int (*dclose)(struct ob_iop_file *f);
    int (*dread)(struct ob_iop_file *f, struct ob_io_dirent *buf);
    int (*getstat)(struct ob_iop_file *f, const char *name, struct ob_io_stat *stat);
    int (*chstat)(struct ob_iop_file *f, const char *name, struct ob_io_stat *stat, unsigned mask);
    /* Extended entries, present only when the device type has OB_IO_DT_FSEXT. */
    int (*rename)(struct ob_iop_file *f, const char *old, const char *new_name);
    int (*chdir)(struct ob_iop_file *f, const char *name);
    int (*sync)(struct ob_iop_file *f, const char *dev, int flag);
    int (*mount)(struct ob_iop_file *f, const char *fsname, const char *devname, int flag,
                 void *arg, int arglen);
    int (*umount)(struct ob_iop_file *f, const char *fsname);
    int64_t (*lseek64)(struct ob_iop_file *f, int64_t offset, int whence);
    int (*devctl)(struct ob_iop_file *f, const char *name, int cmd, void *arg, unsigned arglen,
                  void *buf, unsigned buflen);
    int (*symlink)(struct ob_iop_file *f, const char *old, const char *new_name);
    int (*readlink)(struct ob_iop_file *f, const char *path, char *buf, unsigned buflen);
    int (*ioctl2)(struct ob_iop_file *f, int cmd, void *arg, unsigned arglen, void *buf,
                  unsigned buflen);
};
struct ob_iop_device {
    const char *name;
    unsigned type, version;
    const char *desc;
    struct ob_iop_device_ops *ops;
};

void ob_iop_ioman_init(void);
int ob_io_open(const char *name, int flags);
int ob_io_close(int fd);
int ob_io_read(int fd, void *ptr, int size);
int ob_io_write(int fd, void *ptr, int size);
int ob_io_lseek(int fd, int offset, int whence);
int ob_io_ioctl(int fd, int cmd, void *param);
int ob_io_remove(const char *name);
int ob_io_mkdir(const char *name);
int ob_io_rmdir(const char *name);
int ob_io_dopen(const char *name);
int ob_io_dclose(int fd);
int ob_io_dread(int fd, struct ob_io_dirent *buf);
int ob_io_getstat(const char *name, struct ob_io_stat *stat);
int ob_io_chstat(const char *name, struct ob_io_stat *stat, unsigned mask);
int ob_io_format(const char *name);
int ob_io_add_driver(struct ob_iop_device *d);
void ob_io_share_drivers(int (*add_drv)(struct ob_iop_device *d));
int ob_io_devctl(const char *name, int cmd, void *arg, unsigned arglen, void *buf, unsigned buflen);
int ob_io_del_driver(const char *name);
#endif
