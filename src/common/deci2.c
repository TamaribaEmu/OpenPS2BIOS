/* SPDX-License-Identifier: MIT */
#include "openbios/deci2.h"

void ob_deci2_init(struct ob_deci2 *s, const struct ob_deci2_ops *ops, void *ctx) {
    if (!s) return;
    ob_zero(s, sizeof(*s));
    if (ops) s->ops = *ops;
    s->ctx = ctx;
}
static struct ob_deci2_channel *channel(struct ob_deci2 *s, int32_t id) {
    int i;
    if (id <= 0) return 0;
    for (i = 0; i < OB_DECI2_CHANNELS; ++i)
        if (s->channels[i].id == id) return &s->channels[i];
    return 0;
}
static int32_t open_channel(struct ob_deci2 *s, const uint32_t a[4], uint32_t gp) {
    struct ob_deci2_channel *free_slot = 0;
    uint16_t protocol = (uint16_t)a[0];
    unsigned i;
    if (!protocol || !a[2]) return OB_D2_INVALID;
    /* Protocols 0x0001 and 0x0201 belong to the kernel when no host is attached. */
    if (s->no_route && (protocol == 0x0001 || protocol == 0x0201)) return OB_D2_ALREADYUSE;
    if ((a[2] & 3) || !s->ops.resolve(s->ctx, a[2], 4) ||
        (a[3] && !s->ops.resolve(s->ctx, a[3], 56))) return OB_D2_INVALADDR;
    for (i = 0; i < OB_DECI2_CHANNELS; ++i) {
        struct ob_deci2_channel *c = &s->channels[i];
        if (c->id && c->protocol == protocol) return OB_D2_ALREADYUSE;
        if (!c->id && !free_slot) free_slot = c;
    }
    if (!free_slot) return OB_D2_MFILE;
    do {
        s->next_id = s->next_id >= INT32_MAX ? 1 : s->next_id + 1;
    } while (channel(s, (int32_t)s->next_id));
    ob_zero(free_slot, sizeof(*free_slot));
    free_slot->id = (int32_t)s->next_id;
    free_slot->protocol = protocol;
    free_slot->callback = a[2];
    free_slot->opt = a[1]; /* Opaque caller data; it need not point to readable RAM. */
    free_slot->gp = gp;
    return free_slot->id;
}
static int request_send(struct ob_deci2 *s, struct ob_deci2_channel *c, uint32_t destination) {
    if (s->no_route) return OB_D2_NOROUTE;
    if (c->pending || c->in_callback == OB_D2_WRITE) return OB_D2_WOULDBLOCK;
    /* Only the EE console protocol to the host is routed locally. */
    if (c->protocol != OB_DECI2_TTY_PROTOCOL || (uint8_t)destination != 'H')
        return OB_D2_NOROUTE;
    c->used = c->expected = 0;
    c->error = 0;
    c->pending = 1;
    c->destination = (uint8_t)destination;
    return 0;
}
static int send_bytes(struct ob_deci2 *s, struct ob_deci2_channel *c,
                       uint32_t address, uint32_t length) {
    const uint8_t *data;
    uint32_t total;
    if (!c->pending || c->in_callback != OB_D2_WRITE) return OB_D2_WOULDBLOCK;
    if (c->error) return c->error;
    if (!length) return 0;
    if (length > OB_DECI2_PACKET_MAX - c->used) return c->error = OB_D2_NOSPACE;
    data = s->ops.resolve(s->ctx, address, length);
    if (!data) return c->error = OB_D2_INVALADDR;
    total = c->used + length;
    ob_copy(c->packet + c->used, data, length);
    if (total >= 2) {
        c->expected = ob_le16(c->packet);
        if (c->expected < 12 || c->expected > OB_DECI2_PACKET_MAX || total > c->expected)
            return c->error = OB_D2_PKTSIZE;
    }
    if (total >= 8 && (ob_le16(c->packet + 2) != 0 ||
        ob_le16(c->packet + 4) != c->protocol || c->packet[6] != 'E' ||
        c->packet[7] != c->destination)) return c->error = OB_D2_INVALHEAD;
    c->used = total;
    return (int)length;
}
static void notify(struct ob_deci2 *s, struct ob_deci2_channel *c, int event, int param) {
    c->in_callback = event;
    s->ops.invoke(s->ctx, c->callback, c->gp, event, param, c->opt);
    c->in_callback = 0;
}
static int poll(struct ob_deci2 *s, struct ob_deci2_channel *c) {
    int result;
    if (c->in_callback) return OB_D2_WOULDBLOCK;
    if (!c->pending) return 0;
    notify(s, c, OB_D2_WRITE, OB_DECI2_PACKET_MAX - c->used);
    if (c->error) {
        result = c->error;
        c->pending = 0;
        notify(s, c, OB_D2_ERROR, result);
        return result;
    }
    if (!c->expected || c->used != c->expected) return OB_D2_WOULDBLOCK;
    result = s->ops.output(s->ctx, c->packet + 12, c->used - 12);
    if (result < 0) {
        c->pending = 0;
        notify(s, c, OB_D2_ERROR, result);
        return result;
    }
    c->pending = 0;
    notify(s, c, OB_D2_WRITE_DONE, 0);
    return 0;
}
static int kputs(struct ob_deci2 *s, uint32_t address) {
    uint8_t text[OB_DECI2_PACKET_MAX];
    unsigned i;
    for (i = 0; i < sizeof(text); ++i) {
        const uint8_t *p;
        if (address > UINT32_MAX - i) return OB_D2_INVALADDR;
        p = s->ops.resolve(s->ctx, address + i, 1);
        if (!p) return OB_D2_INVALADDR;
        if (!*p) return s->ops.output(s->ctx, text, i);
        text[i] = *p;
    }
    return OB_D2_PKTSIZE;
}
int32_t ob_deci2_call(struct ob_deci2 *s, int32_t command, uint32_t args, uint32_t gp) {
    uint32_t a[4] = {0, 0, 0, 0};
    const uint8_t *p;
    struct ob_deci2_channel *c;
    unsigned words, i;
    if (!s || !s->ops.resolve || !s->ops.invoke || !s->ops.output) return OB_D2_INVALID;
    switch (command) {
    case 1: words = 4; break;
    case 3: case -7: words = 2; break;
    case -5: case -6: words = 3; break;
    case 2: case 4: case -8: case -9: case 16: words = 1; break;
    default: return OB_D2_INVALID;
    }
    p = s->ops.resolve(s->ctx, args, words * 4);
    if (!p) return OB_D2_INVALADDR;
    for (i = 0; i < words; ++i) a[i] = ob_le32(p + i * 4);
    if (command == 1) return open_channel(s, a, gp);
    if (command == 16) return kputs(s, a[0]);
    c = channel(s, (int32_t)a[0]);
    if (!c) return OB_D2_INVALSOCK;
    switch (command) {
    case 2:
        if (c->pending || c->in_callback || c->locked) return OB_D2_WOULDBLOCK;
        ob_zero(c, sizeof(*c));
        return 0;
    case 3: case -7: return request_send(s, c, a[1]);
    case 4: return poll(s, c);
    case -5:
        if (a[2] && !s->ops.resolve(s->ctx, a[1], a[2])) return OB_D2_INVALADDR;
        return a[2] ? OB_D2_WOULDBLOCK : 0; /* No host input route is present. */
    case -6: return send_bytes(s, c, a[1], a[2]);
    case -8:
        if (c->locked) return OB_D2_ALREADYLOCK;
        c->locked = 1; return 0;
    case -9:
        if (!c->locked) return OB_D2_NOTLOCKED;
        c->locked = 0; return 0;
    default: return OB_D2_INVALID;
    }
}
