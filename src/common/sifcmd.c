/* SPDX-License-Identifier: MIT */
#include "openbios/sif.h"

static int ee_buffer_address(uint32_t address, uint32_t *physical) {
    if ((address >= 0x20000000u && address < 0x22000000u) ||
        (address >= 0x30000000u && address < 0x32000000u)) address &= 0x01ffffffu;
    else if (address >= 0x80000000u && address < 0xc0000000u) address &= 0x1fffffffu;
    if ((address & 15) || address < 0x00080000u ||
        !ob_range(address, 128, 0x02000000u)) return OB_EINVAL;
    *physical = address;
    return OB_OK;
}
int ob_sif_cmd_apply(struct ob_sif_cmd_state *s, const void *packet, uint32_t size) {
    const uint8_t *p = packet;
    uint32_t header, length, command, opt, value;
    if (!s || !p || size < 16 || size > 128) return OB_EINVAL;
    header = ob_le32(p);
    length = header & 255;
    if (length < 16 || length > OB_SIF_CMD_MAX || length > size) return OB_EFORMAT;
    if ((header >> 8) || ob_le32(p + 4)) return OB_ENOSYS; /* Extra data is not supported. */
    command = ob_le32(p + 8);
    opt = ob_le32(p + 12);
    switch (command) {
    case 0x80000000u: /* CHANGE_SADDR */
    case 0x80000002u: /* INIT_CMD, opt=0 advertises the EE command buffer. */
        if (command == 0x80000002u && opt) {
            /* opt=1: the EE asks for RPC readiness; no buffer argument. */
            if (opt != 1) return OB_ENOSYS;
            s->rpc_requested = 1;
            return OB_OK;
        }
        if (length < 20) return OB_EFORMAT;
        if (ee_buffer_address(ob_le32(p + 16), &value)) return OB_EINVAL;
        s->ee_buffer = value;
        return OB_OK;
    case 0x80000001u: /* SET_SREG */
        if (length < 24) return OB_EFORMAT;
        value = ob_le32(p + 16);
        if (value >= 32) return OB_EINVAL;
        s->sregs[value] = ob_le32(p + 20);
        return OB_OK;
    default: return OB_ENOSYS;
    }
}

int ob_sif_cmd_build(void *out, uint32_t capacity, uint32_t command,
                     const uint32_t *payload, uint32_t words) {
    uint8_t *p = out;
    uint32_t i, size = 16 + words * 4;
    if (!p || size > OB_SIF_CMD_MAX || size > capacity || (words && !payload)) return OB_EINVAL;
    ob_zero(p, capacity);
    /* Header: packet size (no extra data), destination 0, command ID, option 0. */
    p[0] = (uint8_t)size;
    for (i = 0; i < 4; ++i) p[8 + i] = (uint8_t)(command >> (8 * i));
    for (i = 0; i < words * 4; ++i) p[16 + i] = (uint8_t)(payload[i / 4] >> (8 * (i % 4)));
    return (int)size;
}
