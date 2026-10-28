/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_DISC_H
#define OPENBIOS_DISC_H
#include "base.h"
/* Private EE/IOP bootstrap mailbox, independent of SIF/RPC.
   state 1/2: sector read request/completion, lba is a sector number; detail
              is the sector count on request (0 or 1: one sector into the
              buffer; up to OB_DISC_BULK_SECTORS: into the bulk buffer) and
              error detail on completion.
   state 5/6: drive status request/completion; detail returns the disc type
              (CDVD register 0x0f) | drive status (0x0a) << 8.
   state 3/4: SIF0 send request/completion, lba is EE physical destination,
              detail is payload quadwords (1..128), buffer holds payload. */
#define OB_DISC_MAGIC 0x4f424344u
#define OB_DISC_MAILBOX_IOP 0xa0042f00u
#define OB_DISC_MAILBOX_EE 0xbc042f00u
#define OB_DISC_BUFFER_IOP 0xa004af00u
#define OB_DISC_BUFFER_EE 0xbc04af00u
#define OB_DISC_BULK_IOP 0xa0046f00u
#define OB_DISC_BULK_EE 0xbc046f00u
#define OB_DISC_BULK_SECTORS 8u
struct ob_disc_mailbox {
    uint32_t magic, state, lba;
    int32_t result;
    uint32_t detail;
};
void ob_iop_disc_service(void) __attribute__((noreturn));
int ob_iop_cd_load_file(const char *path, void **data, uint32_t *size);
struct ob_iso;
/* The disc's ISO9660 volume (mounted on first use), or 0. */
const struct ob_iso *ob_iop_cd_iso(void);
/* The second layer's volume of a dual-layer DVD and its first LSN, or 0. */
const struct ob_iso *ob_iop_cd_iso_layer1(uint32_t *base);
/* Runs fn(arg) on cdvdman's 12 KiB lookup stack, one caller at a time. ISO
   walks keep whole sectors on the stack and module threads are small. */
int ob_iop_cd_on_stack(int (*fn)(void *), void *arg);
int ob_iop_cd_read_sectors(uint32_t lba, uint32_t count, void *dest);
void ob_iop_disc_reboot(void);
/* One drive command for count sectors, delivered in chunk-sector pieces that
   alternate between buf[0] and buf[1]; sink returns nonzero to abort. */
int ob_iop_cd_read_stream(uint32_t lba, uint32_t count, uint8_t *const buf[2], uint32_t chunk,
                          int (*sink)(void *ctx, const uint8_t *data, uint32_t first, uint32_t n),
                          void *ctx, volatile uint32_t *detail);
int ob_iop_cd_find(const char *path, uint32_t *lba, uint32_t *size);
int ob_iop_cd_ncommand(uint8_t cmd, const uint8_t *params, unsigned count);
int ob_iop_cd_read_toc(uint8_t *toc);
/* Whether the disc is a dual-layer DVD and the LSN where layer 1 starts. */
int ob_iop_cd_layer_info(int *dual, uint32_t *layer1);
int ob_iop_cd_read_id(int model, uint32_t *out);
int ob_iop_cd_disc_ready(void);
/* sceCdTrayReq(2): 1 on the first check after cdvdman starts or a disc change. */
int ob_iop_cd_tray_check(void);
int ob_ee_disc_read(void *ctx, uint32_t lba, uint8_t out[2048]);
/* Disc type | drive status << 8, or a negative error (see the mailbox notes). */
int ob_ee_disc_status(void);
/* Forget read-ahead data (call when the disc may have changed). */
void ob_ee_disc_invalidate(void);
int ob_ee_disc_boot(uint32_t *entry);
int ob_ee_rom_load(const char *name, uint32_t *entry);
int ob_ee_disc_load(const char *path, uint32_t *entry);
/* Arguments the next program's SetupThread receives (copied). */
void ob_ee_set_exec_args(int argc, const char *const *argv);
#endif
