/* SPDX-License-Identifier: MIT */
/* IOP memory blocks in 256-byte units over RAM unused by this firmware. */
#include "openbios/iop.h"
#include "openbios/platform.h"

#define BLOCKS 128
static struct { uint32_t address, size; } used[BLOCKS];

extern char __bss_end[];
static uint32_t base;
uint32_t ob_sysmem_base(void) {
    return ((uint32_t)(uintptr_t)__bss_end + 255u) & 0x1fff00u;
}
void ob_sysmem_init(void) {
    ob_zero(used, sizeof(used));
    base = ob_sysmem_base();
    used[BLOCKS - 1].address = OB_SYSMEM_HOLE_START; /* never handed out */
    used[BLOCKS - 1].size = OB_SYSMEM_HOLE_END - OB_SYSMEM_HOLE_START;
}

/* Log every block in table order; used when an allocation fails. */
void ob_sysmem_dump(void) {
    unsigned i;
    ob_puts("[openbios:iop] sysmem blocks:");
    for (i = 0; i < BLOCKS; ++i)
        if (used[i].size) { ob_puts(" "); ob_puthex(used[i].address); ob_puts("+"); ob_puthex(used[i].size); }
    ob_puts("\n");
}
static int overlaps(uint32_t address, uint32_t size) {
    unsigned i;
    for (i = 0; i < BLOCKS; ++i)
        if (used[i].size && address < used[i].address + used[i].size &&
            used[i].address < address + size) return 1;
    return 0;
}
/* Allocation and release run with interrupts off: module threads call
   AllocSysMemory while the loader allocates too, and a thread preempted
   between choosing a slot and filling it overwrote a live module's block
   (MGS3: sdmain was loaded over kcej_sound_basic_lib). */
static uint32_t lock(void) {
    uint32_t s; __asm__ volatile ("mfc0 %0, $12\n\tnop" : "=r"(s));
    __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s & ~1u) : "memory");
    return s;
}
static void unlock(uint32_t s) { __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s) : "memory"); }
static int free_slot(void) {
    unsigned i;
    for (i = 0; i < BLOCKS; ++i) if (!used[i].size) return (int)i;
    return -1;
}
/* Candidate starts are the region ends and the ends/starts of used blocks. */
static void *alloc_locked(int type, uint32_t size, uint32_t address) {
    int slot = free_slot();
    uint32_t best = 0;
    int found = 0;
    unsigned i;
    size = (size + 255) & ~255u;
    if (slot < 0 || !size || size > OB_SYSMEM_END - base) return 0;
    if (type == OB_ALLOC_ADDRESS) {
        address &= 0x1fffffu;
        address &= ~255u;
        if (address < base || address + size > OB_SYSMEM_END || overlaps(address, size))
            return 0;
        best = address; found = 1;
    } else {
        for (i = 0; i <= BLOCKS; ++i) {
            uint32_t start;
            if (i == BLOCKS) start = type == OB_ALLOC_LAST ? OB_SYSMEM_END - size : base;
            else if (!used[i].size) continue;
            else if (type == OB_ALLOC_LAST) {
                if (used[i].address < base + size) continue;
                start = used[i].address - size;
            } else start = used[i].address + used[i].size;
            if (start < base || start + size > OB_SYSMEM_END || overlaps(start, size))
                continue;
            if (!found || (type == OB_ALLOC_LAST ? start > best : start < best)) best = start;
            found = 1;
        }
    }
    if (!found) return 0;
    used[slot].address = best;
    used[slot].size = size;
    return (void *)(uintptr_t)best;
}
void *ob_sysmem_alloc(int type, uint32_t size, uint32_t address) {
    uint32_t s = lock();
    void *p = alloc_locked(type, size, address);
    unlock(s);
    return p;
}
int ob_sysmem_free(void *block) {
    uint32_t address = (uint32_t)(uintptr_t)block & 0x1fffffu, s = lock();
    unsigned i;
    for (i = 0; i < BLOCKS - 1; ++i)
        if (used[i].size && used[i].address == address) {
            used[i].size = 0;
            unlock(s);
            return KE_OK;
        }
    unlock(s);
    return KE_ILLEGAL_MEMBLOCK;
}
uint32_t ob_sysmem_free_size(void) {
    uint32_t total = OB_SYSMEM_END - base;
    unsigned i;
    for (i = 0; i < BLOCKS; ++i) total -= used[i].size;
    return total;
}

/* Largest free block: walk the free gaps between used blocks (the reserved
   window is a used block too). */
uint32_t ob_sysmem_largest_free(void) {
    uint32_t start = base, best = 0;
    for (;;) {
        uint32_t next = OB_SYSMEM_END, end = 0;
        unsigned i;
        int inside = 0;
        for (i = 0; i < BLOCKS; ++i) {
            if (!used[i].size) continue;
            if (used[i].address <= start && start < used[i].address + used[i].size) {
                inside = 1;
                if (used[i].address + used[i].size > end) end = used[i].address + used[i].size;
            } else if (used[i].address > start && used[i].address < next) next = used[i].address;
        }
        if (inside) { start = end; continue; }
        if (next - start > best) best = next - start;
        if (next >= OB_SYSMEM_END) return best;
        start = next;
    }
}
/* The used block containing address: its start and size, or 0. */
uint32_t ob_sysmem_block(uint32_t address, uint32_t *size) {
    unsigned i;
    address &= 0x1fffffu;
    for (i = 0; i < BLOCKS - 1; ++i)
        if (used[i].size && address >= used[i].address && address < used[i].address + used[i].size) {
            if (size) *size = used[i].size;
            return used[i].address;
        }
    return 0;
}
