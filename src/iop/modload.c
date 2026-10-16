/* SPDX-License-Identifier: MIT */
/* modload: load IRX modules from any ioman device or from memory, and start
   them. Encrypted KELF modules are not supported; the KELF path callback is
   only stored for modules that register one. */
#include "openbios/iop.h"
#include "openbios/ioman.h"

int ob_iop_register_builtin(struct ob_irx_exports *exports, uint32_t count);

static uint32_t kelf_path_callback;

/* Read a whole file through ioman into a new sysmem block. */
static int read_file(const char *name, void **data, uint32_t *size) {
    int fd = ob_io_open(name, OB_IO_O_RDONLY), n;
    void *block;
    if (fd < 0) return KE_NOFILE;
    n = ob_io_lseek(fd, 0, OB_IO_SEEK_END);
    if (n <= 0 || ob_io_lseek(fd, 0, OB_IO_SEEK_SET) != 0) { ob_io_close(fd); return KE_FILEERR; }
    block = ob_sysmem_alloc(OB_ALLOC_LAST, (uint32_t)n + 16, 0);
    if (!block) { ob_io_close(fd); return KE_NO_MEMORY; }
    if (ob_io_read(fd, block, n) != n) { ob_io_close(fd); ob_sysmem_free(block); return KE_FILEERR; }
    ob_io_close(fd);
    *data = block;
    *size = (uint32_t)n;
    return KE_OK;
}
static int LoadModule(const char *name) {
    void *file;
    uint32_t size;
    int r = read_file(name, &file, &size);
    if (r) return r;
    r = ob_iop_load_module_only(file, size, name);
    ob_sysmem_free(file);
    return r;
}
static int LoadStartModule(const char *name, int arglen, const char *args, int *result) {
    void *file;
    uint32_t size;
    int r = read_file(name, &file, &size);
    if (r) return r;
    /* The file is released before the module starts: start routines often
       allocate most of the remaining memory in one block. */
    r = ob_iop_load_module_only(file, size, name);
    ob_sysmem_free(file);
    if (r < 0) return r;
    return ob_iop_start_module(r, name, args, arglen > 0 ? (uint32_t)arglen : 0, result);
}
static int StartModule(int id, const char *name, int arglen, const char *args, int *result) {
    return ob_iop_start_module(id, name, args, arglen > 0 ? (uint32_t)arglen : 0, result);
}
static int LoadModuleBuffer(void *buffer) {
    /* The size is not passed; the IRX parser bounds itself by its headers. */
    return ob_iop_load_module_only(buffer, 0x7fffffffu, "(buffer)");
}
/* LoadModuleBufferAddress(buffer, address, offset): link the image to run at
   address, memory the caller has already set aside. */
static int LoadModuleBufferAddress(void *buffer, void *address, int offset) {
    uint32_t at = (uint32_t)(uintptr_t)address + (uint32_t)offset;
    if (!address) return ob_iop_load_module_only(buffer, 0x7fffffffu, "(buffer)");
    return ob_iop_load_module_at(buffer, 0x7fffffffu, "(buffer)", at);
}
/* LoadModuleWithOption(path, option): option = {position, access, reserved[2],
   distaddr, distoffset, ...}. Position 2 places the module at distaddr +
   distoffset (caller-provided memory); other positions load anywhere. Custom
   file functions are not supported. */
struct load_option { uint8_t position, access, reserved[2]; uint32_t distaddr; int distoffset; uint32_t functable; };
static int LoadModuleWithOption(const char *name, const struct load_option *option) {
    void *file;
    uint32_t size;
    int r = read_file(name, &file, &size);
    if (r) return r;
    if (option && option->position == 2 && option->distaddr)
        r = ob_iop_load_module_at(file, size, name, option->distaddr + (uint32_t)option->distoffset);
    else r = ob_iop_load_module_only(file, size, name);
    ob_sysmem_free(file);
    return r;
}
/* AllocLoadMemory(type, size, address) / FreeLoadMemory(block): memory for a
   later LoadModuleBufferAddress/LoadModuleWithOption at a chosen place. */
static void *AllocLoadMemory(int type, uint32_t size, void *address) {
    return ob_sysmem_alloc(type, size, (uint32_t)(uintptr_t)address);
}
static int FreeLoadMemory(void *block) { return ob_sysmem_free(block); }
static void SetCheckKelfPathCallback(uint32_t fn) { kelf_path_callback = fn; }
static void GetLoadfileCallbacks(uint32_t *check_kelf_path, uint32_t *set_loadfile_callbacks) {
    if (check_kelf_path) *check_kelf_path = kelf_path_callback;
    if (set_loadfile_callbacks) *set_loadfile_callbacks = 0;
}
static int IsIllegalBootDevice(const char *path) { (void)path; return 0; }
static int GetModuleIdList(int *ids, int capacity, int *total) {
    return ob_iop_module_ids(0, ids, capacity, total);
}
static int ReferModuleStatus(int id, struct ob_module_status *status) {
    return ob_iop_module_status(id, status);
}
static int GetModuleIdListByName(const char *name, int *ids, int capacity, int *total) {
    return ob_iop_module_ids(name, ids, capacity, total);
}
static int SearchModuleByName(const char *name) { return ob_iop_module_search(name, 0); }
static int SearchModuleByAddress(const void *address) {
    return ob_iop_module_search(0, (uint32_t)(uintptr_t)address);
}
static int StopModule(int id, int arglen, const char *args, int *result) {
    return ob_iop_stop_module(id, args, arglen > 0 ? (uint32_t)arglen : 0, result);
}
static int UnloadModule(int id) { return ob_iop_unload_module(id); }
static int ret_zero(void) { return 0; }

static struct { uint32_t magic, next; uint16_t version, mode; char name[8] __attribute__((nonstring));
                uint32_t f[31]; } modload = {OB_IRX_EXPORT_MAGIC, 0, 0x0107, 0, "modload", {0}};
#define FN(p) ((uint32_t)(uintptr_t)(p))
void ob_iop_modload_init(void) {
    kelf_path_callback = 0;
    modload.f[0] = modload.f[1] = modload.f[2] = modload.f[3] = FN(ret_zero);
    modload.f[6] = FN(LoadModule);
    modload.f[7] = FN(LoadStartModule);
    modload.f[8] = FN(StartModule);
    modload.f[9] = FN(LoadModuleBufferAddress);
    modload.f[10] = FN(LoadModuleBuffer);
    modload.f[13] = FN(SetCheckKelfPathCallback);
    modload.f[14] = FN(GetLoadfileCallbacks);
    modload.f[15] = FN(IsIllegalBootDevice);
    modload.f[16] = FN(GetModuleIdList);
    modload.f[19] = FN(LoadModuleWithOption);
    modload.f[17] = FN(ReferModuleStatus);
    modload.f[18] = FN(GetModuleIdListByName);
    modload.f[20] = FN(StopModule);     modload.f[21] = FN(UnloadModule);
    modload.f[22] = FN(SearchModuleByName);
    modload.f[28] = FN(AllocLoadMemory);  modload.f[29] = FN(FreeLoadMemory);
    modload.f[23] = FN(SearchModuleByAddress);
    ob_iop_register_builtin((void *)&modload, 30);
}
