/* SPDX-License-Identifier: MIT */
#include "openbios/sifdma.h"

#define TAG_REFE 0u
#define TAG_REF 3u

void ob_sifdma_init(struct ob_sifdma_queue *q, uint32_t physical_base) {
    ob_zero(q, sizeof(*q));
    q->base = physical_base;
    q->active = -1;
    q->pending = 0;
    q->next_sequence = 1; /* Keeps every ID nonzero, including count-zero IDs. */
}
int ob_sifdma_check(const struct ob_sifdma_xfer *x, uint32_t count) {
    uint32_t i;
    if ((!x && count) || count > OB_SIFDMA_MAX) return OB_EINVAL;
    for (i = 0; i < count; ++i) {
        uint32_t bytes;
        /* Zero is accepted; negative sizes would overrun memory. */
        if (x[i].size < 0 || (uint32_t)x[i].size > OB_SIFDMA_MAX_BYTES) return OB_EBOUNDS;
        /* DMA moves whole qwords; the rounded tail must also be in range. */
        bytes = ((uint32_t)x[i].size + 15) & ~15u;
        if ((x[i].src & 15) || !ob_range(x[i].src, bytes, 0x02000000u)) return OB_EINVAL;
        if ((x[i].dest & 3) || !ob_range(x[i].dest, bytes, OB_SIFDMA_IOP_RAM)) return OB_EINVAL;
    }
    return OB_OK;
}
static uint32_t field(const struct ob_sifdma_queue *q, const void *p) {
    return q->base + (uint32_t)((const uint8_t *)p - (const uint8_t *)q);
}
uint32_t ob_sifdma_submit(struct ob_sifdma_queue *q, const struct ob_sifdma_xfer *x,
                          uint32_t count, int *start) {
    struct ob_sifdma_batch *b = &q->batch[q->pending];
    uint32_t i, first = b->count;
    *start = 0;
    if (ob_sifdma_check(x, count) || first + count > OB_SIFDMA_MAX) return 0;
    if (!count) return (uint32_t)q->next_sequence << 16 | first << 8;
    if (!first) b->sequence = q->next_sequence;
    else b->ee_tags[first * 2 - 1][0] |= TAG_REF << 28; /* Previous end now continues. */
    for (i = 0; i < count; ++i) {
        uint32_t n = first + i, qwc = ((uint32_t)x[i].size + 15) >> 4;
        uint32_t *iop = b->iop_tags[n], *ref = b->ee_tags[n * 2], *data = ref + 4;
        iop[0] = (x[i].dest & 0x00ffffffu) |
            (x[i].attr & OB_SIFDMA_INT_O ? 0x80000000u : 0) |
            (x[i].attr & OB_SIFDMA_ERT ? 0x40000000u : 0);
        iop[1] = qwc * 4; /* The IOP counts 32-bit words. */
        iop[2] = iop[3] = 0;
        ref[0] = 1 | TAG_REF << 28;
        ref[1] = field(q, iop);
        ref[2] = ref[3] = 0;
        /* INT_I sets the EE tag IRQ bit. The channel runs without TIE, so the
           chain continues and completion is reported once at its end. */
        data[0] = qwc | (x[i].attr & OB_SIFDMA_INT_I ? 0x80000000u : 0) |
            (i + 1 < count ? TAG_REF : TAG_REFE) << 28;
        data[1] = x[i].src;
        data[2] = data[3] = 0;
    }
    b->count = first + count;
    if (q->active < 0) {
        q->active = q->pending;
        q->pending ^= 1;
        q->batch[q->pending].count = 0;
        ++q->next_sequence;
        *start = 1;
    }
    return (uint32_t)b->sequence << 16 | first << 8 | count;
}
uint32_t ob_sifdma_active_tags(const struct ob_sifdma_queue *q) {
    return q->active < 0 ? 0 : field(q, q->batch[q->active].ee_tags);
}
int ob_sifdma_complete(struct ob_sifdma_queue *q) {
    if (q->active < 0) return 0;
    q->batch[q->active].count = 0;
    q->active = -1;
    if (!q->batch[q->pending].count) return 0;
    q->active = q->pending;
    q->pending ^= 1;
    q->batch[q->pending].count = 0;
    ++q->next_sequence;
    return 1;
}
int ob_sifdma_status(const struct ob_sifdma_queue *q, uint32_t id) {
    uint16_t sequence = (uint16_t)(id >> 16);
    if (!(id & 0xff)) return -1; /* Count-zero IDs transfer nothing. */
    if (q->batch[q->pending].count && q->batch[q->pending].sequence == sequence) return 1;
    if (q->active >= 0 && q->batch[q->active].sequence == sequence) return 0;
    return -1;
}
