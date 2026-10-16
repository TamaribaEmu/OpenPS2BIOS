/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_SIFDMA_H
#define OPENBIOS_SIFDMA_H
#include "base.h"

/* Public SifDmaTransfer_t attribute bits. */
#define OB_SIFDMA_INT_I 0x02u
#define OB_SIFDMA_INT_O 0x04u
#define OB_SIFDMA_ERT 0x40u
#define OB_SIFDMA_ATTRS (OB_SIFDMA_INT_I | OB_SIFDMA_INT_O | OB_SIFDMA_ERT)
/* At most 31 descriptors per call. Bytes per descriptor are limited by the
   16-bit EE QWC. */
#define OB_SIFDMA_MAX 31u
#define OB_SIFDMA_MAX_BYTES 0xffff0u
#define OB_SIFDMA_IOP_RAM 0x00200000u

/* Descriptor after address translation: src is EE physical, dest IOP physical. */
struct ob_sifdma_xfer {
    uint32_t src, dest;
    int32_t size;
    uint32_t attr;
};
/* EE source-chain tags for one batch. Each descriptor uses a REF tag that sends
   its 16-byte IOP destination tag, then a REF/REFE tag that sends the payload. */
struct ob_sifdma_batch {
    uint32_t ee_tags[OB_SIFDMA_MAX * 2][4];
    uint32_t iop_tags[OB_SIFDMA_MAX][4];
    uint32_t count;
    uint16_t sequence;
} __attribute__((aligned(16)));
struct ob_sifdma_queue {
    struct ob_sifdma_batch batch[2];
    uint32_t base; /* Physical address of this structure. */
    int active;    /* Batch index started on hardware, or -1. */
    int pending;   /* Batch index collecting submissions. */
    uint16_t next_sequence;
};

void ob_sifdma_init(struct ob_sifdma_queue *q, uint32_t physical_base);
/* Validate translated descriptors. Returns OB_OK or an error without side effects.
   Sources must already be qword aligned; unknown attribute bits are ignored. */
int ob_sifdma_check(const struct ob_sifdma_xfer *x, uint32_t count);
/* Queue checked descriptors. Returns a nonzero transfer ID, or zero when full.
   A count of zero queues nothing and returns an ID that reports completion.
   *start is set when the caller must start the returned batch on hardware. */
uint32_t ob_sifdma_submit(struct ob_sifdma_queue *q, const struct ob_sifdma_xfer *x,
                          uint32_t count, int *start);
/* Physical address of the first tag of the active batch. */
uint32_t ob_sifdma_active_tags(const struct ob_sifdma_queue *q);
/* The active chain finished. Returns 1 when a queued batch
   became active and must be started. */
int ob_sifdma_complete(struct ob_sifdma_queue *q);
/* >0 queued, 0 transferring, <0 completed or unknown. */
int ob_sifdma_status(const struct ob_sifdma_queue *q, uint32_t id);
#endif
