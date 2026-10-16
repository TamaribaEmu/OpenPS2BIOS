/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_DECI2_H
#define OPENBIOS_DECI2_H
#include "base.h"

#define OB_DECI2_CHANNELS 8
#define OB_DECI2_PACKET_MAX 4096
#define OB_DECI2_TTY_PROTOCOL 0x0210u
enum ob_deci2_error {
    OB_D2_INVALID = -1, OB_D2_INVALSOCK = -2, OB_D2_ALREADYUSE = -3,
    OB_D2_MFILE = -4, OB_D2_INVALADDR = -5, OB_D2_PKTSIZE = -6,
    OB_D2_WOULDBLOCK = -7, OB_D2_ALREADYLOCK = -8, OB_D2_NOTLOCKED = -9,
    OB_D2_NOROUTE = -10, OB_D2_NOSPACE = -11, OB_D2_INVALHEAD = -12
};
enum ob_deci2_event { OB_D2_WRITE = 3, OB_D2_WRITE_DONE = 4, OB_D2_ERROR = 6 };
struct ob_deci2_ops {
    void *(*resolve)(void *ctx, uint32_t address, uint32_t size);
    void (*invoke)(void *ctx, uint32_t callback, uint32_t gp,
                   int event, int param, uint32_t opt);
    /* Synchronous, all-or-error sink: zero means all bytes were delivered. */
    int (*output)(void *ctx, const uint8_t *bytes, uint32_t size);
};
struct ob_deci2_channel {
    int32_t id;
    uint16_t protocol;
    uint8_t destination;
    uint32_t callback, gp, opt, used, expected;
    int pending, locked, in_callback, error;
    uint8_t packet[OB_DECI2_PACKET_MAX];
};
struct ob_deci2 {
    struct ob_deci2_ops ops;
    void *ctx;
    /* Set: act as a console with no debugger attached, refusing every send
       with NOROUTE. Clear: route protocol 0x0210 E->H to the local console. */
    int no_route;
    uint32_t next_id;
    struct ob_deci2_channel channels[OB_DECI2_CHANNELS];
};
void ob_deci2_init(struct ob_deci2 *s, const struct ob_deci2_ops *ops, void *ctx);
int32_t ob_deci2_call(struct ob_deci2 *s, int32_t command, uint32_t args, uint32_t gp);
#endif
