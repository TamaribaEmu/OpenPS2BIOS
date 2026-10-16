/* SPDX-License-Identifier: MIT */
/* heaplib: growable heaps over sysmem blocks. There is no locking; callers
   serialize access. */
#include "openbios/iop.h"
#include "openbios/iopk.h"

int ob_iop_register_builtin(struct ob_irx_exports *exports, uint32_t count);

static void *block_alloc(uint32_t size, int high) {
    return ob_sysmem_alloc(high ? OB_ALLOC_LAST : OB_ALLOC_FIRST, size, 0);
}
static void block_free(void *p) { ob_sysmem_free(p); }
static const struct obk_mem_ops ops = { block_alloc, block_free };

static void *CreateHeap(int block_size, int flag) { return obk_heap_create(&ops, block_size, flag); }
static void DeleteHeap(void *heap) { obk_heap_delete(&ops, heap); }
static void *AllocHeapMemory(void *heap, uint32_t size) { return obk_heap_alloc(&ops, heap, size); }
static int FreeHeapMemory(void *heap, void *p) { return obk_heap_free(&ops, heap, p); }
static int HeapTotalFreeSize(void *heap) { return obk_heap_free_size(heap); }
static void HeapPrepare(void *mem, int size) { if (size > 0) obk_chunk_init(mem, (uint32_t)size); }
static int HeapChunkSize(void *mem) {
    struct obk_chunk *c = obk_chunk_at(mem);
    return c ? (int)obk_chunk_free_size(c) : 0;
}

#define FN(p) ((uint32_t)(uintptr_t)(p))
static int ret_zero(void) { return 0; }
static struct { uint32_t magic, next; uint16_t version, mode;
                char name[8] __attribute__((nonstring)); uint32_t f[17]; } heaplib = {
    OB_IRX_EXPORT_MAGIC, 0, 0x0101, 0, "heaplib", {0}};

void ob_iop_heaplib_init(void) {
    unsigned i;
    for (i = 0; i < 4; ++i) heaplib.f[i] = FN(ret_zero);
    heaplib.f[4] = FN(CreateHeap);      heaplib.f[5] = FN(DeleteHeap);
    heaplib.f[6] = FN(AllocHeapMemory); heaplib.f[7] = FN(FreeHeapMemory);
    heaplib.f[8] = FN(HeapTotalFreeSize);
    heaplib.f[11] = FN(HeapPrepare);    heaplib.f[15] = FN(HeapChunkSize);
    ob_iop_register_builtin((void *)&heaplib, 16);
}
