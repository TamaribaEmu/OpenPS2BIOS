/* SPDX-License-Identifier: MIT */
/* secrman subset: memory card authentication. All cryptography happens in the
   MECHACON (S-commands 0x80-0x8f) and the card; this module only exchanges
   8-byte blocks between them in MagicGate order. The card is reached through
   the command handler that mcman registers. */
#include "openbios/iop.h"
#include "openbios/platform.h"

int ob_iop_register_builtin(struct ob_irx_exports *exports, uint32_t count);
int ob_cd_scommand(uint8_t cmd, const void *params, unsigned count, void *result, unsigned want);

/* sio2man transfer descriptor. */
struct sio2_dma_arg { void *addr; int size, count; };
struct sio2_transfer {
    uint32_t stat6c, port_ctrl1[4], port_ctrl2[4], stat70, regdata[16], stat74;
    uint32_t in_size, out_size;
    uint8_t *in, *out;
    struct sio2_dma_arg in_dma, out_dma;
};
typedef int (*card_handler)(int port, int slot, struct sio2_transfer *t);
typedef int (*devid_handler)(int port, int slot);
static card_handler command_handler;
/* Kept but unused: SecrAuthCard receives cnum directly. */
static devid_handler id_handler __attribute__((unused));
static uint32_t command_gp;
static uint32_t id_gp __attribute__((unused));

static uint32_t current_gp(void) { uint32_t g; __asm__ volatile ("move %0, $28" : "=r"(g)); return g; }
static void SecrSetMcCommandHandler(card_handler h) { command_handler = h; command_gp = current_gp(); }
static void SecrSetMcDevIDHandler(devid_handler h) { id_handler = h; id_gp = current_gp(); }

static int send_card(int port, int slot, struct sio2_transfer *t) {
    uint32_t saved;
    int r;
    __asm__ volatile ("move %0, $28" : "=r"(saved));
    __asm__ volatile ("move $28, %0" :: "r"(command_gp));
    r = command_handler(port, slot, t);
    __asm__ volatile ("move $28, %0" :: "r"(saved));
    return r;
}
static uint8_t xor_sum(const uint8_t *p, unsigned n) {
    uint8_t c = 0;
    while (n--) c ^= *p++;
    return c;
}
/* One card command 0x81 <group> <command> [8 data bytes + checksum]. kind:
   0 = plain (5-byte frame), 1 = write 8 bytes, 2 = read 8 bytes. */
static int card(int port, int slot, int group, int command, int kind, uint8_t block[8]) {
    struct sio2_transfer t;
    uint8_t in[14], out[14];
    unsigned n = kind ? 14 : 5, i;
    ob_zero(&t, sizeof(t));
    ob_zero(in, sizeof(in));
    ob_zero(out, sizeof(out));
    t.port_ctrl1[port & 3] = 0xff060505u;
    t.port_ctrl2[port & 3] = 0x0003ffffu;
    t.regdata[0] = (uint32_t)(port & 3) | (kind ? 0x00380e40u : 0x00140540u);
    t.in = in;
    t.out = out;
    t.in_size = t.out_size = n;
    in[0] = 0x81;
    in[1] = (uint8_t)group;
    in[2] = (uint8_t)command;
    if (kind == 1) {
        for (i = 0; i < 8; ++i) in[3 + i] = block[7 - i]; /* Sent most significant byte first. */
        in[11] = xor_sum(block, 8);
    }
    if (!send_card(port, slot, &t)) return 0;
    if ((t.stat6c >> 13) & 1 || (t.stat6c >> 14) & 3) return 0; /* SIO2 error bits */
    if (kind == 1) return out[12] == 0x2b && out[13] != 0x66;
    if (out[3] != 0x2b) return 0;
    if (kind == 0) return out[4] != 0x66;
    if (out[13] == 0x66 || xor_sum(&out[4], 8) != out[12]) return 0;
    for (i = 0; i < 8; ++i) block[7 - i] = out[4 + i];
    return 1;
}
static int mecha(uint8_t cmd, const uint8_t *in, unsigned n, uint8_t out[16]) {
    uint8_t r[16];
    unsigned i;
    for (i = 0; i < 16; ++i) r[i] = 0;
    if (ob_cd_scommand(cmd, in, n, r, 16) < 1 || r[0]) return 0;
    if (out) for (i = 0; i < 16; ++i) out[i] = r[i];
    return 1;
}
/* MECHACON busy polling: status 1 means still calculating. */
static int mecha_wait(void) {
    uint8_t r[16];
    unsigned i;
    for (i = 0; i < 100000; ++i) {
        if (ob_cd_scommand(0x8f, 0, 0, r, 16) < 1) return 0;
        if (r[0] != 1) return r[0] == 0;
    }
    return 0;
}

static int SecrAuthCard(int port, int slot, int cnum) {
    uint8_t iv[8], material[8], nonce[8], challenge1[8], challenge2[8], challenge3[8];
    uint8_t response1[8], response2[8], response3[8], param[16], r[16];
    unsigned i;
    uint8_t c = (uint8_t)cnum;
    if (!command_handler) {
        ob_puts("[openbios:iop] secrman: no memory card command handler\n");
        return 0;
    }
    /* Restart the card's authentication state, then MECHACON's. */
    if (!card(port, slot, 0xf3, 0x00, 0, 0)) return 0;
    if (!mecha(0x80, &c, 1, 0)) return 0;
    if (!card(port, slot, 0xf7, 0x01, 0, 0)) goto fail_card; /* key change, command 1 */
    if (!card(port, slot, 0xf0, 0x00, 0, 0)) goto fail_card;
    if (!mecha(0x81, &c, 1, 0)) goto fail_card;
    if (!card(port, slot, 0xf0, 0x01, 2, iv)) goto fail;
    if (!card(port, slot, 0xf0, 0x02, 2, material)) goto fail;
    for (i = 0; i < 8; ++i) { param[i] = iv[i]; param[8 + i] = material[i]; }
    if (!mecha(0x82, param, 16, 0)) goto fail;
    if (!card(port, slot, 0xf0, 0x03, 0, 0)) goto fail;
    if (!card(port, slot, 0xf0, 0x04, 2, nonce)) goto fail;
    if (!mecha(0x83, nonce, 8, 0)) goto fail;
    if (!card(port, slot, 0xf0, 0x05, 0, 0)) goto fail;
    if (!mecha_wait()) goto fail;
    if (!mecha(0x84, 0, 0, r)) goto fail;
    for (i = 0; i < 8; ++i) challenge1[i] = r[1 + i];
    for (i = 0; i < 4; ++i) challenge2[i] = r[9 + i];
    if (!mecha(0x85, 0, 0, r)) goto fail;
    for (i = 0; i < 4; ++i) challenge2[4 + i] = r[1 + i];
    for (i = 0; i < 8; ++i) challenge3[i] = r[5 + i];
    if (!card(port, slot, 0xf0, 0x06, 1, challenge3)) goto fail;
    if (!card(port, slot, 0xf0, 0x07, 1, challenge2)) goto fail;
    if (!card(port, slot, 0xf0, 0x08, 0, 0)) goto fail;
    if (!card(port, slot, 0xf0, 0x09, 0, 0)) goto fail;
    if (!card(port, slot, 0xf0, 0x0a, 0, 0)) goto fail;
    if (!card(port, slot, 0xf0, 0x0b, 1, challenge1)) goto fail;
    if (!card(port, slot, 0xf0, 0x0c, 0, 0)) goto fail;
    if (!card(port, slot, 0xf0, 0x0d, 0, 0)) goto fail;
    if (!card(port, slot, 0xf0, 0x0e, 0, 0)) goto fail;
    if (!card(port, slot, 0xf0, 0x0f, 2, response1)) goto fail;
    if (!card(port, slot, 0xf0, 0x10, 0, 0)) goto fail;
    if (!card(port, slot, 0xf0, 0x11, 2, response2)) goto fail;
    for (i = 0; i < 8; ++i) { param[i] = response1[i]; param[8 + i] = response2[i]; }
    if (!mecha(0x86, param, 16, 0)) goto fail;
    if (!card(port, slot, 0xf0, 0x12, 0, 0)) goto fail;
    if (!card(port, slot, 0xf0, 0x13, 2, response3)) goto fail;
    if (!mecha(0x87, response3, 8, 0)) goto fail;
    if (!mecha_wait()) goto fail;
    if (!card(port, slot, 0xf0, 0x14, 0, 0)) goto fail;
    if (!mecha(0x88, 0, 0, 0)) goto fail;
    return 1;
fail:
    card(port, slot, 0xf3, 0x00, 0, 0);
    mecha(0x80, &c, 1, 0);
    return 0;
fail_card:
    card(port, slot, 0xf3, 0x00, 0, 0);
    return 0;
}
static void SecrResetAuthCard(int port, int slot, int cnum) {
    uint8_t c = (uint8_t)cnum;
    if (command_handler) card(port, slot, 0xf3, 0x00, 0, 0);
    mecha(0x80, &c, 1, 0);
}

static int ret_zero(void) { return 0; }
static struct { uint32_t magic, next; uint16_t version, mode; char name[8] __attribute__((nonstring));
                uint32_t f[22]; } secrman = {OB_IRX_EXPORT_MAGIC, 0, 0x0103, 0, "secrman", {0}};
#define FN(p) ((uint32_t)(uintptr_t)(p))
void ob_iop_secrman_init(void) {
    command_handler = 0;
    id_handler = 0;
    secrman.f[0] = secrman.f[1] = secrman.f[2] = secrman.f[3] = FN(ret_zero);
    secrman.f[4] = FN(SecrSetMcCommandHandler);
    secrman.f[5] = FN(SecrSetMcDevIDHandler);
    secrman.f[6] = FN(SecrAuthCard);
    secrman.f[7] = FN(SecrResetAuthCard);
    ob_iop_register_builtin((void *)&secrman, 21);
}
