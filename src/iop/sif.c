/* SPDX-License-Identifier: MIT */
#include "openbios/sif.h"
#include "openbios/platform.h"
#include "openbios/iop.h"
#include "openbios/disc.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SIF1_CHCR REG32(0xbf801538u)
static struct ob_sif_cmd_state command;
static void sif_interrupt_init(void);

static void arm(void) {
    REG32(0xbf801534u) = 0;
    __asm__ volatile ("nop" ::: "memory");
    SIF1_CHCR = 0x01000700u; /* Destination chain, header transfer, start. */
}
static uint32_t sif_lock(void) {
    uint32_t v; __asm__ volatile ("mfc0 %0, $12\n\tnop" : "=r"(v));
    __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(v & ~1u) : "memory");
    return v;
}
static void sif_unlock(uint32_t v) { __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(v) : "memory"); }
/* EE physical address for DMA: user aliases and KSEG0/1 normalized. */
uint32_t ob_ee_physical(uint32_t a) {
    if (a >= 0x20000000u && a < 0x40000000u) return a & 0x01ffffffu;
    return a & 0x1fffffffu;
}
/* IOP -> EE transfer on DMA channel 9. Each segment has a 16-byte IOP tag whose
   upper half is the EE destination-chain tag that TTE sends ahead of the data.
   Tags live on the caller's stack, so concurrent senders never share them.
   Thread senders are served in call order (tickets): a client may give two
   RPCs the same receive buffer and rely on the replies arriving in order.
   Senders in interrupt context just spin until the channel is idle. */
static volatile int channel_owner, owner_thread;
static volatile uint32_t next_ticket, serving;
static volatile int ticket_thread[16]; /* -1: that sender was terminated */
/* Games terminate their streaming threads (Family Guy, on a movie skip), maybe
   in the middle of a send. Drop that thread's claim on the channel and its
   place in line, or every later sender waits for it. */
void ob_iop_sif_thread_gone(int id) {
    unsigned i;
    if (channel_owner && owner_thread == id) channel_owner = 0;
    for (i = 0; i < 16; ++i)
        if (ticket_thread[i] == id) ticket_thread[i] = -1;
}
#ifdef OB_TRACE_RPC
uint32_t ob_iop_trace_t[3];
#endif
static int interrupts_enabled(void) {
    uint32_t v; __asm__ volatile ("mfc0 %0, $12\n\tnop" : "=r"(v));
    return (v & 1u) != 0;
}
int ob_iop_sif0_send_chain(const struct ob_sif0_segment *seg, unsigned count) {
    uint32_t tags[4 * 4 + 4] __attribute__((aligned(16)));
    uint32_t lock, i, waited, ticket = 0;
    int thread = interrupts_enabled();
    if (!seg || !count || count > 4) return OB_EINVAL;
    for (i = 0; i < count; ++i) {
        uint32_t dest = ob_ee_physical(seg[i].ee_destination);
        if (!seg[i].qwc || seg[i].qwc > 0xffff || (seg[i].iop_source & 3) || (dest & 15) ||
            dest < 0x00080000u || !ob_range(dest, seg[i].qwc * 16, 0x02000000u)) return OB_EINVAL;
        tags[i * 4] = (seg[i].iop_source & 0x1fffffu) | (i + 1 == count ? 0x80000000u : 0);
        tags[i * 4 + 1] = seg[i].qwc * 4;
        tags[i * 4 + 2] = 0x10000000u | (seg[i].irq ? 0x80000000u : 0) | seg[i].qwc; /* CNT */
        tags[i * 4 + 3] = dest;
    }
#ifdef OB_TRACE_RPC
    ob_iop_trace_t[0] = (uint32_t)(ob_iop_system_time() / 36864u);
#endif
    if (thread) {
        lock = sif_lock();
        ticket = next_ticket++;
        ticket_thread[ticket & 15] = ob_kernel_current_thread();
        sif_unlock(lock);
    }
    for (waited = 0;; ++waited) {
        lock = sif_lock();
        if (thread && serving != ticket && ticket_thread[serving & 15] == -1) {
            ticket_thread[serving & 15] = 0;
            ++serving;
        }
        if (!thread ? !(REG32(0xbf801528u) & 0x01000000u)
                    : serving == ticket && !channel_owner && !(REG32(0xbf801528u) & 0x01000000u)) break;
        sif_unlock(lock);
        /* Yield to the owner or to senders ahead in line, which may be lower
           priority. Only the head of the line may time out. */
        if (thread && waited > 100 && ob_kernel_pause(1000)) {
            if (waited > 10000u && serving == ticket) { ++serving; return OB_EBUSY; }
        } else if (waited > 200000000u && (!thread || serving == ticket)) {
            if (thread) ++serving;
            return OB_EBUSY;
        }
    }
#ifdef OB_TRACE_RPC
    ob_puts("[openbios:iop] sif0 start waited="); ob_puthex(waited); ob_puts(" thread="); ob_puthex((uint32_t)thread);
    ob_puts(" dest="); ob_puthex(tags[3]); ob_puts("\n");
#endif
    if (thread) {
        channel_owner = 1;
        owner_thread = ticket_thread[ticket & 15];
        ticket_thread[ticket & 15] = 0;
        ++serving;
    }
    REG32(0xbf801570u) |= 0x800; /* IOP DMA9 enable. */
    REG32(0xbf801578u) |= 1; /* Global IOP DMA enable. */
    REG32(0xbf801520u) = 0;
    REG32(0xbf801524u) = 0;
    REG32(0xbf80152cu) = (uint32_t)(uintptr_t)tags & 0x1fffffu;
    __asm__ volatile ("nop" ::: "memory");
    REG32(0xbf801528u) = 0x01000701u;
    sif_unlock(lock);
#ifdef OB_TRACE_RPC
    ob_iop_trace_t[1] = (uint32_t)(ob_iop_system_time() / 36864u);
#endif
    /* The tags are on this stack: wait until the transfer has consumed them. */
    for (i = 0; i < 400000000u && (REG32(0xbf801528u) & 0x01000000u); ++i) {}
    if (thread) channel_owner = 0;
#ifdef OB_TRACE_RPC
    ob_puts("[openbios:iop] sif0 end chcr="); ob_puthex(REG32(0xbf801528u)); ob_puts("\n");
    ob_iop_trace_t[2] = (uint32_t)(ob_iop_system_time() / 36864u);
#endif
    return (REG32(0xbf801528u) & 0x01000000u) ? OB_EBUSY : OB_OK;
}
int ob_iop_sif0_send(uint32_t iop_source, uint32_t ee_destination, uint32_t qwc) {
    struct ob_sif0_segment seg = {iop_source, ee_destination, qwc, 1};
    if (qwc > 128) return OB_EINVAL;
    return ob_iop_sif0_send_chain(&seg, 1);
}
uint32_t ob_iop_sif_sreg(unsigned index, int write, uint32_t value) {
    if (index >= 32) return 0;
    if (write) command.sregs[index] = value;
    return command.sregs[index];
}
int ob_iop_sif_ee_buffer(uint32_t *ee_buffer) {
    if (!command.ee_buffer) return OB_EINVAL;
    *ee_buffer = command.ee_buffer;
    return OB_OK;
}
/* Once the EE has requested RPC initialization and its command buffer is
   known, announce readiness with SET_SREG(RPCINIT, 1). */
static void announce_rpc(void) {
    static const uint32_t sreg[2] = {OB_SIF_SREG_RPCINIT, 1};
    int size;
#ifdef OB_TRACE_RPC
    ob_puts("[openbios:iop] announce req="); ob_puthex(command.rpc_requested); ob_puts(" done=");
    ob_puthex(command.rpc_announced); ob_puts(" buf="); ob_puthex(command.ee_buffer); ob_puts("\n");
#endif
    if (!command.rpc_requested || command.rpc_announced || !command.ee_buffer) return;
    size = ob_sif_cmd_build((void *)OB_SIF0_OUT_IOP, OB_SIF0_OUT_MAX, 0x80000001u, sreg, 2);
    if (size < 0) return;
    if (ob_iop_sif0_send(OB_SIF0_OUT_IOP, command.ee_buffer, ((uint32_t)size + 15) / 16) == OB_OK)
        command.rpc_announced = 1;
    else ob_puts("[openbios:iop] RPC init announcement failed\n");
}
/* Each module in an IOPRP image carries its 8-character name followed by its
   4-digit SDK release ("loadfile2420", or "...." in some images). LOADFILE's
   GET_VERSION reply is that release; read it from the image named after
   "UDNL ". */
static void loadfile_version_from_image(const char *arg) {
    char path[80];
    const char *p = arg;
    void *data;
    uint32_t size, i, n = 0;
    while (*p && !(p[0] == 'U' && p[1] == 'D' && p[2] == 'N' && p[3] == 'L' && p[4] == ' ')) ++p;
    if (!*p) return;
    p += 5;
    while (*p == ' ') ++p;
    while (p[n] && p[n] != ' ' && n < sizeof(path) - 1) { path[n] = p[n]; ++n; }
    path[n] = 0;
    if (!n) return;
    i = (uint32_t)ob_iop_cd_load_file(path, &data, &size);
    if (i) {
        ob_puts("[openbios:iop] IOPRP image not readable: "); ob_puts(path); ob_puts(" error=");
        ob_puthex(i); ob_puts("\n");
        return;
    }
    for (i = 0; i + 12 <= size; ++i) {
        const char *t = (const char *)data + i;
        unsigned k, ok = 1;
        for (k = 8; k < 12; ++k) ok &= (t[k] >= '0' && t[k] <= '9') || t[k] == '.';
        if (ok && t[0] == 'l' && t[1] == 'o' && t[2] == 'a' && t[3] == 'd' && t[4] == 'f' &&
            t[5] == 'i' && t[6] == 'l' && t[7] == 'e') {
            ob_copy(ob_iop_loadfile_version, t + 8, 4);
            ob_puts("[openbios:iop] LOADFILE release from image: "); ob_puts(path); ob_puts("\n");
            break;
        }
    }
    ob_sysmem_free(data);
}
/* RESET_CMD (SifCmdResetData_t: header, arglen, mode, arg[80]). The modules
   named by the argument are not loaded; instead the built-in services are
   restarted: SIF command/RPC state is cleared and the handshake flags are
   raised again, ending with BOOTEND. */
static void reboot(const uint8_t *packet, uint32_t bytes) {
    char text[81];
    uint32_t n = bytes >= 24 ? ob_le32(packet + 16) : 0, k;
    unsigned i;
    if (n > 80) n = 80;
    if (n > bytes - 24) n = bytes > 24 ? bytes - 24 : 0;
    for (k = 0; k < n; ++k) {
        uint8_t c = packet[24 + k];
        text[k] = (char)(c >= 32 && c < 127 ? c : '.');
    }
    text[n] = 0;
    /* LOADFILE's GET_VERSION string must match the SDK release of the IOPRP
       image; clients compare it exactly. Guess from the name (IOPRP255 ->
       "2550") until the image's own tag has been read. */
    for (k = 0; k + 8 <= n; ++k)
        if (text[k] == 'I' && text[k + 1] == 'O' && text[k + 2] == 'P' && text[k + 3] == 'R' &&
            text[k + 4] == 'P' && text[k + 5] >= '0' && text[k + 5] <= '9' &&
            text[k + 6] >= '0' && text[k + 6] <= '9' && text[k + 7] >= '0' && text[k + 7] <= '9') {
            ob_iop_loadfile_version[0] = text[k + 5];
            ob_iop_loadfile_version[1] = text[k + 6];
            ob_iop_loadfile_version[2] = text[k + 7];
            ob_iop_loadfile_version[3] = '0';
            break;
        }
    ob_puts("[openbios:iop] reboot requested arg=\""); ob_puts(text);
    ob_puts("\" mode="); ob_puthex(bytes >= 24 ? ob_le32(packet + 20) : 0); ob_puts("\n");
    /* The EE clears SIFINIT/CMDINIT, and usually BOOTEND, after sending the
       reset; wait for that so the new flags are not lost. Libraries that leave
       BOOTEND set are given up to about 200 ms. */
    for (i = 0; i < 20000000 && (REG32(0xbd000030u) & 0x30000); ++i) {}
    for (i = 0; i < 200 && (REG32(0xbd000030u) & 0x40000); ++i) ob_kernel_pause(1000);
    ob_zero(&command, sizeof(command));
    /* Server threads die with the reboot, possibly mid-transfer: let a SIF0
       send finish and release the channel and drive they may hold. */
    for (i = 0; i < 20000000 && (REG32(0xbf801528u) & 0x01000000u); ++i) {}
    channel_owner = 0;
    serving = next_ticket; /* Waiting senders died with their threads. */
    ob_iop_disc_reboot();
    ob_iop_rpc_reset();
    loadfile_version_from_image(text);
    sif_interrupt_init();
    REG32(0xbd000010u) = OB_SIF_CMD_BUFFER_IOP & 0x1fffffu;
    REG32(0xbd000030u) = 0x70000; /* SIFINIT | CMDINIT | BOOTEND */
    ob_puts("[openbios:iop] reboot complete (native services; requested modules not loaded)\n");
}
/* IOP DMA channel 10 (SIF1 receive) completion: wake the service thread. */
static int sif1_interrupt(void *arg) {
    (void)arg;
    ob_kernel_wakeup(1);
    return 1;
}
static void sif_interrupt_init(void) {
    ob_intr_register(0x2b, sif1_interrupt, 0);
    ob_intr_enable(0x2b, 1);
}
void ob_iop_sif_init(void) {
    struct ob_sif_status *status = (void *)OB_SIF_STATUS_IOP;
    ob_zero(&command, sizeof(command));
    ob_zero((void *)OB_SIF_CMD_BUFFER_IOP, 128);
    ob_zero(status, sizeof(*status));
    REG32(0xbf801570u) |= 0x8000; /* IOP DMA10 enable. */
    REG32(0xbf801578u) |= 1;
    arm();
    /* Publish a live receive buffer before advertising SIF/CMD readiness. */
    REG32(0xbd000010u) = OB_SIF_CMD_BUFFER_IOP & 0x1fffffu;
    REG32(0xbd000030u) = 0x30000;
    __asm__ volatile ("nop" ::: "memory");
    status->magic = OB_SIF_STATUS_MAGIC;
    sif_interrupt_init();
}
/* System commands this endpoint implements itself; everything else goes to
   handlers that modules registered with sceSifAddCmdHandler. */
static int native_command(uint32_t cid) {
    switch (cid) {
    case 0x80000000u: case 0x80000001u: case 0x80000002u: case 0x80000003u:
    case 0x80000008u: case 0x80000009u: case 0x8000000au: case 0x8000000cu:
        return 1;
    default:
        return 0;
    }
}
/* EE sceSifGetOtherData: {rec_id, pkt_addr, rpc_id, client, IOP source, EE
   destination, size} after the header. Copy the data to the EE, then answer
   with an RPC_END naming the request (public sifrpc RDATA layout). */
static void other_data(const uint8_t *packet) {
    uint32_t end[12] __attribute__((aligned(16)));
    struct ob_sif0_segment seg[2];
    uint32_t src = ob_le32(packet + 32), dest = ob_le32(packet + 36), size = ob_le32(packet + 40);
    unsigned n = 0, k;
    ob_zero(end, sizeof(end));
    end[0] = 48 | (size << 8);
    end[1] = dest;
    end[2] = OB_SIF_CMD_RPC_END;
    for (k = 0; k < 4; ++k) end[4 + k] = ob_le32(packet + 16 + 4 * k);
    end[8] = 0x8000000cu;
    end[9] = src;
    end[10] = dest;
    end[11] = size;
    if (size) {
        seg[n].iop_source = src & 0x1fffffu;
        seg[n].ee_destination = dest;
        seg[n].qwc = (size + 15) / 16;
        seg[n++].irq = 0;
    }
    seg[n].iop_source = (uint32_t)(uintptr_t)end;
    seg[n].qwc = 3;
    seg[n].irq = 1;
    if (ob_iop_sif_ee_buffer(&seg[n++].ee_destination)) return;
    if (ob_iop_sif0_send_chain(seg, n) != OB_OK) {
        ob_puts("[openbios:iop] RDATA send failed src="); ob_puthex(src); ob_puts(" dest=");
        ob_puthex(dest); ob_puts(" size="); ob_puthex(size); ob_puts("\n");
    }
}
int ob_iop_sif_poll(void) {
    struct ob_sif_status *status = (void *)OB_SIF_STATUS_IOP;
    uint8_t packet[128];
    uint32_t end, bytes;
    int result;
    if (SIF1_CHCR & 0x01000000u) return 0;
    end = REG32(0xbf801530u);
    /* A command transfer must have ended inside the published buffer. */
    if (end < (OB_SIF_CMD_BUFFER_IOP & 0x1fffffu) ||
        end > (OB_SIF_CMD_BUFFER_IOP & 0x1fffffu) + sizeof(packet)) {
        ob_puts("[openbios:iop] invalid SIF1 command DMA destination\n");
        ob_halt();
    }
    bytes = end - (OB_SIF_CMD_BUFFER_IOP & 0x1fffffu);
    ob_copy(packet, (void *)OB_SIF_CMD_BUFFER_IOP, bytes);
    ob_zero((void *)OB_SIF_CMD_BUFFER_IOP, 128);
    arm();
    {
        struct ob_rpc_request rpc;
        if (bytes >= 12 && ob_rpc_parse(packet, bytes, &rpc) == OB_OK) {
            ob_iop_rpc_handle(&rpc);
            status->last_command = rpc.command;
            status->result = OB_OK;
            status->received_bytes = bytes;
            status->last_argument = rpc.command == OB_SIF_CMD_RPC_BIND ? rpc.sid : rpc.rpc_number;
            ++status->received;
            return 1;
        }
    }
    if (bytes >= 44 && ob_le32(packet + 8) == 0x8000000cu) {
        other_data(packet);
        status->last_command = 0x8000000cu;
        status->result = OB_OK;
        status->received_bytes = bytes;
        ++status->received;
        return 1;
    }
    if (bytes >= 48 && ob_le32(packet + 8) == OB_SIF_CMD_RPC_END) {
        uint32_t words[12];
        unsigned k;
        for (k = 0; k < 12; ++k) words[k] = ob_le32(packet + 4 * k);
        if (!ob_iop_module_rpc_end(words))
            ob_puts("[openbios:iop] RPC_END for unknown client\n");
        status->last_command = OB_SIF_CMD_RPC_END;
        status->result = OB_OK;
        status->received_bytes = bytes;
        ++status->received;
        return 1;
    }
    if (bytes >= 16 && !native_command(ob_le32(packet + 8)) &&
        ob_iop_module_command(packet, ob_le32(packet + 8))) {
        status->last_command = ob_le32(packet + 8);
        status->result = OB_OK;
        status->received_bytes = bytes;
        ++status->received;
        return 1;
    }
    if (bytes >= 12 && ob_le32(packet + 8) == OB_SIF_CMD_RESET) {
        reboot(packet, bytes);
        status->last_command = OB_SIF_CMD_RESET;
        status->result = OB_OK;
        status->received_bytes = bytes;
        ++status->received;
        return 1;
    }
    result = ob_sif_cmd_apply(&command, packet, bytes);
#ifdef OB_TRACE_RPC
    ob_puts("[openbios:iop] syscmd "); ob_puthex(ob_le32(packet + 8)); ob_puts(" opt=");
    ob_puthex(ob_le32(packet + 12)); ob_puts(" result="); ob_puthex((uint32_t)result); ob_puts("\n");
#endif
    if (!result && ob_le32(packet + 8) == 0x80000002u && !ob_le32(packet + 12))
        REG32(0xbd000030u) = 0x20000; /* Acknowledge command-buffer initialization. */
    if (!result) announce_rpc();
    status->last_command = bytes >= 12 ? ob_le32(packet + 8) : 0;
    status->result = result;
    status->received_bytes = bytes;
    status->last_argument = bytes >= 20 ? ob_le32(packet + 16) : 0;
    status->command = command;
    __asm__ volatile ("nop" ::: "memory");
    ++status->received;
    if (result) {
        /* Commands without a handler are dropped; log each ID once. */
        static uint32_t reported[8];
        unsigned k;
        for (k = 0; k < 8 && reported[k] && reported[k] != status->last_command; ++k) {}
        if (k < 8 && !reported[k]) {
            reported[k] = status->last_command;
            ob_puts("[openbios:iop] dropped SIF command="); ob_puthex(status->last_command);
            ob_puts(" error="); ob_puthex((uint32_t)result); ob_puts(" (reported once)\n");
        }
    }
    return 1;
}
