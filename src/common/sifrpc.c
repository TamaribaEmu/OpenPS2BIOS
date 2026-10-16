/* SPDX-License-Identifier: MIT */
#include "openbios/sif.h"

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
int ob_rpc_parse(const void *packet, uint32_t size, struct ob_rpc_request *r) {
    const uint8_t *p = packet;
    uint32_t header, length, need;
    if (!p || !r || size < 16 || size > 128) return OB_EINVAL;
    header = ob_le32(p);
    length = header & 255;
    if (length < 16 || length > OB_SIF_CMD_MAX || length > size) return OB_EFORMAT;
    ob_zero(r, sizeof(*r));
    r->command = ob_le32(p + 8);
    r->extra_size = header >> 8;
    r->extra_dest = ob_le32(p + 4);
    if (r->command == OB_SIF_CMD_RPC_BIND) need = 36;
    else if (r->command == OB_SIF_CMD_RPC_CALL) need = 56;
    else return OB_ENOSYS;
    if (length < need) return OB_EFORMAT;
    r->rec_id = ob_le32(p + 16);
    r->pkt_addr = ob_le32(p + 20);
    r->rpc_id = ob_le32(p + 24);
    r->client = ob_le32(p + 28);
    if (r->command == OB_SIF_CMD_RPC_BIND) {
        r->sid = ob_le32(p + 32);
        return OB_OK;
    }
    r->rpc_number = ob_le32(p + 32);
    r->send_size = ob_le32(p + 36);
    r->recvbuf = ob_le32(p + 40);
    r->recv_size = ob_le32(p + 44);
    r->rmode = ob_le32(p + 48);
    r->server = ob_le32(p + 52);
    return OB_OK;
}
int ob_rpc_build_end(void *out, uint32_t capacity, const struct ob_rpc_request *r,
                     uint32_t server, uint32_t buffer, uint32_t cbuffer) {
    uint8_t *p = out;
    if (!p || !r || capacity < 48) return OB_EINVAL;
    if (r->command != OB_SIF_CMD_RPC_BIND && r->command != OB_SIF_CMD_RPC_CALL) return OB_EINVAL;
    ob_zero(p, capacity);
    p[0] = 48;
    put32(p + 8, OB_SIF_CMD_RPC_END);
    put32(p + 16, r->rec_id);
    put32(p + 20, r->pkt_addr);
    put32(p + 24, r->rpc_id);
    put32(p + 28, r->client);
    put32(p + 32, r->command);
    put32(p + 36, server);
    put32(p + 40, buffer);
    put32(p + 44, cbuffer);
    return 48;
}
