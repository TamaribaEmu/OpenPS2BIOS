/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_SIF_H
#define OPENBIOS_SIF_H
#include "base.h"

#define OB_SIF_CMD_MAX 112u
#define OB_SIF_CMD_BUFFER_IOP 0xa0044f00u
#define OB_SIF_STATUS_IOP 0xa0045000u
#define OB_SIF_STATUS_EE 0xbc045000u
#define OB_SIF_STATUS_MAGIC 0x4f425346u
struct ob_sif_cmd_state {
    uint32_t ee_buffer;
    uint32_t sregs[32];
    uint32_t rpc_requested; /* INIT_CMD opt=1 received from the EE. */
    uint32_t rpc_announced; /* SET_SREG(RPCINIT, 1) delivered over SIF0. */
};
#define OB_SIF_SREG_RPCINIT 0u
/* IOP-side staging for outgoing SIF0 packets (tag at 0x91000, payload after). */
#define OB_SIF0_TAG_IOP 0xa0043f00u
#define OB_SIF0_OUT_IOP 0xa0043f40u
#define OB_SIF0_OUT_MAX 128u
/* Private IOP status block for the EE; not part of the SIF interface. */
struct ob_sif_status {
    uint32_t magic, received, last_command;
    int32_t result;
    struct ob_sif_cmd_state command;
    uint32_t received_bytes, last_argument;
};
int ob_sif_cmd_apply(struct ob_sif_cmd_state *s, const void *packet, uint32_t size);
/* Build a system command packet (16-byte header plus payload words). Returns the
   packet size in bytes, or an error. */
int ob_sif_cmd_build(void *out, uint32_t capacity, uint32_t command,
                     const uint32_t *payload, uint32_t words);
int ob_iop_sif0_send(uint32_t iop_source, uint32_t ee_destination, uint32_t qwc);
void ob_iop_sif_init(void);
/* Service one received SIF1 command; 0 when none is pending. */
int ob_iop_sif_poll(void);

/* RPC packets (public SifRpc*Pkt_t layouts; word offsets after the 16-byte header). */
#define OB_SIF_CMD_RPC_END 0x80000008u
#define OB_SIF_CMD_RPC_BIND 0x80000009u
#define OB_SIF_CMD_RPC_CALL 0x8000000au
#define OB_SIF_CMD_RESET 0x80000003u
struct ob_rpc_request {
    uint32_t command, rec_id, pkt_addr, rpc_id, client;
    uint32_t sid;                         /* BIND */
    uint32_t rpc_number, send_size, recvbuf, recv_size, rmode, server; /* CALL */
    uint32_t extra_size, extra_dest;      /* header extra-data fields */
};
int ob_rpc_parse(const void *packet, uint32_t size, struct ob_rpc_request *r);
/* Build the RPC_END reply (48 bytes) for a BIND or CALL request. */
int ob_rpc_build_end(void *out, uint32_t capacity, const struct ob_rpc_request *r,
                     uint32_t server, uint32_t buffer, uint32_t cbuffer);
void ob_iop_rpc_handle(const struct ob_rpc_request *r);
void ob_iop_rpc_reset(void);
/* One IOP -> EE transfer segment. EE tags are CNT; irq sets the tag IRQ bit,
   which stops the EE receiver (TIE) and raises its interrupt. */
struct ob_sif0_segment { uint32_t iop_source, ee_destination, qwc, irq; };
int ob_iop_sif0_send_chain(const struct ob_sif0_segment *segments, unsigned count);
int ob_iop_sif_ee_buffer(uint32_t *ee_buffer);
/* A terminated thread gives up its place in the SIF0 send queue. */
void ob_iop_sif_thread_gone(int id);
uint32_t ob_ee_physical(uint32_t address);
uint32_t ob_iop_sif_sreg(unsigned index, int write, uint32_t value);
/* Send an RPC_END for a request (see rpc.c). Thread-safe. */
void ob_iop_rpc_reply(const struct ob_rpc_request *r, uint32_t server, uint32_t buffer,
                      uint32_t cbuffer, uint32_t data_source, uint32_t data_bytes);
/* Module RPC registry (sifrpc.c): returns 1 when the request was handled. */
int ob_iop_module_rpc(const struct ob_rpc_request *r);
/* Module command handlers (sifrpc.c): returns 1 when a handler ran. */
int ob_iop_module_command(void *packet, uint32_t cid);
int ob_iop_module_rpc_end(const uint32_t *packet);
#endif
