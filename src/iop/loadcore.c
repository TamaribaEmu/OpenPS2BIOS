/* SPDX-License-Identifier: MIT */
/* Library registry and module loader for IRX modules. */
#include "openbios/iop.h"
#include "openbios/ioman.h"
#include "openbios/format.h"
#include "openbios/platform.h"

#define MAX_LIBRARIES 64
#define MAX_MODULES 64
#define MAX_ARGS 16

struct library { struct ob_irx_exports *exports; uint32_t count; };
static struct library libraries[MAX_LIBRARIES];
static struct module {
    int id, started;
    uint32_t base, size, entry, gp;
    uint32_t text_size, data_size, bss_size;
    uint16_t version;
    int owned; /* image memory allocated by the loader (not caller-provided) */
    int removable; /* started with REMOVABLE_RESIDENT_END: may be stopped */
    char name[32];
} modules[MAX_MODULES];
static int next_module_id;

/* The services built into this firmware, listed as resident modules under
   their usual module names (public PS2SDK IRX_ID names) in boot order, so
   module lists and name searches see what a retail IOP has loaded. R&C 3's
   IOP_stash_daemon counts the "cdvd" modules in that list. */
static const struct { const char *name; uint16_t version; } residents[] = {
    {"System_Memory_Manager", 0x0203}, {"Module_Manager", 0x0206},
    {"Interrupt_Manager", 0x0101}, {"System_C_lib", 0x0101}, {"Heap_lib", 0x0101},
    {"Multi_Thread_Manager", 0x0203}, {"Vblank_service", 0x0101},
    {"IO/File_Manager", 0x0101}, {"Moldule_File_loader", 0x0209}, {"Stdio", 0x0101},
    {"IOP_SIF_manager", 0x0205}, {"IOP_SIF_rpc_interface", 0x0208},
    {"cdvd_driver", 0x0226}, {"cdvd_ee_driver", 0x0226}, {"LoadModuleByEE", 0x0202},
    {"FILEIO_service", 0x0101}, {"Timer_Manager", 0x0202}, {"dmacman", 0x0101},
};
void ob_loadcore_init(void) {
    unsigned i, n;
    ob_zero(libraries, sizeof(libraries));
    ob_zero(modules, sizeof(modules));
    next_module_id = 1;
    for (i = 0; i < sizeof(residents) / sizeof(residents[0]); ++i) {
        struct module *m = &modules[i];
        for (n = 0; residents[i].name[n] && n < sizeof(m->name) - 1; ++n) m->name[n] = residents[i].name[n];
        m->version = residents[i].version;
        m->id = next_module_id++;
        m->started = 1; /* resident: can't be stopped or unloaded */
    }
}
static int same_name(const char *a, const char *b) {
    unsigned i;
    for (i = 0; i < 8; ++i) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}
/* Module export tables may hold zero placeholders between entries, so they
   are not cut at the first zero; an import reads the entry at its index and
   only a zero entry is unresolved. */
static uint32_t count_functions(const struct ob_irx_exports *e) {
    (void)e;
    return 1024;
}
/* Register a library. Built-in tables may contain null (unimplemented)
   entries, so their explicit size is passed through count. */
static int add_library(struct ob_irx_exports *exports, uint32_t count) {
    unsigned i, slot = MAX_LIBRARIES;
    if (!exports || exports->magic != OB_IRX_EXPORT_MAGIC) return KE_ILLEGAL_LIBRARY;
    for (i = 0; i < MAX_LIBRARIES; ++i) {
        if (!libraries[i].exports) { if (slot == MAX_LIBRARIES) slot = i; continue; }
        if (same_name(libraries[i].exports->name, exports->name) &&
            libraries[i].exports->version == exports->version) return KE_LIBRARY_FOUND;
    }
    if (slot == MAX_LIBRARIES) return KE_ERROR;
    libraries[slot].exports = exports;
    libraries[slot].count = count;
    return KE_OK;
}
int ob_iop_register_library(struct ob_irx_exports *exports) {
    if (!exports || exports->magic != OB_IRX_EXPORT_MAGIC) return KE_ILLEGAL_LIBRARY;
    return add_library(exports, count_functions(exports));
}
int ob_iop_register_builtin(struct ob_irx_exports *exports, uint32_t count) {
    return add_library(exports, count);
}
int ob_iop_release_library(struct ob_irx_exports *exports) {
    unsigned i;
    for (i = 0; i < MAX_LIBRARIES; ++i)
        if (libraries[i].exports == exports) {
            libraries[i].exports = 0;
            return KE_OK;
        }
    return KE_LIBRARY_NOTFOUND;
}

/* QueryLibraryEntryTable: the function table of the registered library named
   by query (an export-table-shaped header), or 0. IOMANX uses it to hook the
   older ioman's exports. */
uint32_t *ob_iop_query_library(const struct ob_irx_exports *query) {
    unsigned i;
    for (i = 0; i < MAX_LIBRARIES; ++i) {
        struct ob_irx_exports *e = libraries[i].exports;
        if (e && same_name(e->name, query->name) && ob_irx_version_ok(query->version, e->version))
            return e->functions;
    }
    return 0;
}

static uint32_t resolve(void *ctx, const char name[8], uint16_t version, uint32_t index) {
    unsigned i;
    (void)ctx;
    for (i = 0; i < MAX_LIBRARIES; ++i) {
        const struct ob_irx_exports *e = libraries[i].exports;
        if (!e || !same_name(e->name, name) || !ob_irx_version_ok(version, e->version)) continue;
        if (index >= libraries[i].count || !e->functions[index]) return 0;
        /* Exports and module code share the low 256 MiB region j can reach. */
        if (e->functions[index] & 0xf0000000u) return 0;
        return e->functions[index];
    }
    return 0;
}

static void log_link_error(const char *path, const uint8_t *image, uint32_t table, uint32_t index) {
    char text[96], lib[9];
    unsigned k;
    for (k = 0; k < 8; ++k) lib[k] = (char)image[table + 12 + k];
    lib[8] = 0;
    ob_format(text, sizeof(text), "[openbios:iop] %s: unresolved import %s v%u.%02u #%u\n",
              path, lib, image[table + 9], image[table + 8], index);
    ob_puts(text);
}

/* Drop a module: libraries registered inside its image, then its memory. */
static void release_module(struct module *m) {
    unsigned i;
    for (i = 0; i < MAX_LIBRARIES; ++i) {
        uint32_t at = (uint32_t)(uintptr_t)libraries[i].exports;
        if (libraries[i].exports && at >= m->base && at < m->base + m->size)
            libraries[i].exports = 0;
    }
    if (m->owned) ob_sysmem_free((void *)(uintptr_t)m->base);
    ob_zero(m, sizeof(*m));
}
static struct module *find_module(int id) {
    unsigned i;
    for (i = 0; i < MAX_MODULES; ++i) if (id && modules[i].id == id) return &modules[i];
    return 0;
}
/* Retail starts each module on a new loader thread, whose unused stack still
   holds CreateThread's 0xff fill. Some start routines read stack they never
   wrote (IOP_stash_daemon), so give them the same. */
static void fill_free_stack(void) {
    uint32_t sp, base = ob_kernel_stack_base();
    __asm__ volatile ("move %0, $sp" : "=r"(sp));
    if (base && base + 64 < sp - 64) {
        volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)((base + 3) & ~3u);
        while ((uint32_t)(uintptr_t)w < sp - 64) *w++ = 0xffffffffu;
    }
}
/* IOMANX keeps its own device table; on Sony firmware it takes over the
   drivers the ROM's IOMAN registered. Once a module exporting iomanx is
   resident, hand it this firmware's drivers through its AddDrv (#20). */
static void share_drivers(const struct module *m) {
    unsigned i;
    for (i = 0; i < MAX_LIBRARIES; ++i) {
        const struct ob_irx_exports *e = libraries[i].exports;
        uint32_t at = (uint32_t)(uintptr_t)e;
        if (e && at >= m->base && at < m->base + m->size && same_name(e->name, "iomanx") &&
            e->functions[20])
            ob_io_share_drivers((int (*)(struct ob_iop_device *))(uintptr_t)e->functions[20]);
    }
}
/* Run a loaded module's entry point with argv = {path, args...}. A module
   that does not stay resident is unloaded; returns its ID either way.
   Like retail, the path and the args share one stack buffer, so argv[1]
   follows the path directly. Some modules read their binary args at a
   fixed offset from argv[1] (R&C 3's IOP_stash_daemon). Modules start
   other modules from their entry point, so the buffer can't be static. */
static int start_module(struct module *m, const char *path, const char *args, uint32_t args_len,
                        int *modres) {
    char *strings, *argv[MAX_ARGS + 1];
    int argc = 0, result, id = m->id;
    uint32_t i, path_len = 0, end;
    while (path[path_len] && path_len < 255) ++path_len;
    if (args_len > 255) args_len = 255;
    strings = __builtin_alloca((path_len + args_len + 2 + 7) & ~7u);
    ob_copy(strings, path, path_len);
    strings[path_len] = 0;
    i = path_len + 1;
    if (args_len) ob_copy(strings + i, args, args_len);
    end = i + args_len;
    strings[end] = 0;
    argv[argc++] = strings;
    while (i < end && argc < MAX_ARGS) {
        argv[argc++] = strings + i;
        while (i < end && strings[i]) ++i;
        ++i;
    }
    argv[argc] = 0;
    m->started = 1;
    {
        int service = ob_kernel_current_thread() == 1;
        int old = service ? ob_kernel_set_priority(OB_IOP_MODULE_START_PRIORITY) : 0;
        fill_free_stack();
        result = ob_iop_call_module(m->entry, m->gp, argc, argv);
        if (service) ob_kernel_set_priority(old);
    }
    if (modres) *modres = result;
    m->removable = (result & 3) == OB_MODULE_REMOVABLE_END;
    if ((result & 3) == OB_MODULE_NO_RESIDENT_END) release_module(m);
    else share_drivers(m);
    return id;
}
/* StopModule: the entry runs again with a negative argc; a
   module that agrees returns NO_RESIDENT_END and stays loaded but stopped. */
int ob_iop_stop_module(int id, const char *args, uint32_t args_len, int *modres) {
    static char arg_copy[256];
    char *argv[MAX_ARGS + 1];
    int argc = 0, result;
    uint32_t i;
    struct module *m = find_module(id);
    if (!m) return KE_UNKNOWN_MODULE;
    if (!m->started) return KE_NOT_STARTED;
    if (!m->removable) return KE_NOT_REMOVABLE;
    if (args_len > sizeof(arg_copy) - 1) args_len = sizeof(arg_copy) - 1;
    if (args_len) ob_copy(arg_copy, args, args_len);
    arg_copy[args_len] = 0;
    argv[argc++] = m->name;
    for (i = 0; i < args_len && argc < MAX_ARGS; ) {
        argv[argc++] = arg_copy + i;
        while (i < args_len && arg_copy[i]) ++i;
        ++i;
    }
    argv[argc] = 0;
    result = ob_iop_call_module(m->entry, m->gp, -argc, argv);
    if (modres) *modres = result;
    if ((result & 3) != OB_MODULE_NO_RESIDENT_END) return KE_CAN_NOT_STOP;
    m->started = 0;
    return id;
}
/* UnloadModule: a stopped (or never started) module's memory and libraries. */
int ob_iop_unload_module(int id) {
    struct module *m = find_module(id);
    if (!m) return KE_UNKNOWN_MODULE;
    if (m->started) return KE_NOT_STOPPED;
    release_module(m);
    return id;
}
/* Load and link an IRX image; start it when start is set. */
static int load_module(const void *file, uint32_t size, const char *path, const char *args,
                       uint32_t args_len, int *modres, int start, uint32_t at) {
    struct ob_irx_info info;
    struct module *m = 0;
    uint8_t *image;
    uint32_t failed_table = 0, failed_index = 0, base, i;
    int result;
    char text[128];
    if (modres) *modres = 0;
    if (ob_irx_parse(file, size, &info)) return KE_ILLEGAL_OBJECT;
    for (i = 0; i < MAX_MODULES; ++i) if (!modules[i].id) { m = &modules[i]; break; }
    if (!m) return KE_NO_MEMORY;
    /* at: memory the caller provides (LoadModuleBufferAddress); else allocate. */
    image = at ? (uint8_t *)(uintptr_t)(at & 0x1fffffu) : ob_sysmem_alloc(OB_ALLOC_FIRST, info.mem_size + 16, 0);
    if (!image) return KE_NO_MEMORY;
    base = (uint32_t)(uintptr_t)image;
    if (ob_irx_load(file, size, &info, image, base)) {
        if (!at) ob_sysmem_free(image);
        return KE_ILLEGAL_OBJECT;
    }
    result = ob_irx_link(image, info.mem_size & ~3u, resolve, 0, &failed_table, &failed_index);
    if (result < 0) {
        if (result == OB_ENOTFOUND) log_link_error(path, image, failed_table, failed_index);
        if (!at) ob_sysmem_free(image);
        return KE_LINKERR;
    }
    m->id = next_module_id++;
    m->started = 0;
    m->owned = !at;
    m->base = base;
    m->size = info.mem_size;
    m->entry = base + info.entry;
    m->gp = info.gp ? base + info.gp : 0;
    m->text_size = info.text_size;
    m->data_size = info.data_size;
    m->bss_size = info.bss_size;
    m->version = info.version;
    ob_copy(m->name, info.name, sizeof(m->name));
    ob_format(text, sizeof(text), "[openbios:iop] module %s (%s v%u.%02u) id %d at %06x size %x\n",
              path, info.name, info.version >> 8, info.version & 0xff, m->id, base, info.mem_size);
    ob_puts(text);
    return start ? start_module(m, path, args, args_len, modres) : m->id;
}
int ob_iop_load_module(const void *file, uint32_t size, const char *path,
                       const char *args, uint32_t args_len, int *modres) {
    return load_module(file, size, path, args, args_len, modres, 1, 0);
}
int ob_iop_load_module_only(const void *file, uint32_t size, const char *path) {
    return load_module(file, size, path, 0, 0, 0, 0, 0);
}
int ob_iop_load_module_at(const void *file, uint32_t size, const char *path, uint32_t at) {
    return load_module(file, size, path, 0, 0, 0, 0, at);
}
int ob_iop_start_module(int id, const char *path, const char *args, uint32_t args_len, int *modres) {
    unsigned i;
    for (i = 0; i < MAX_MODULES; ++i)
        if (modules[i].id == id && id) {
            if (modules[i].started) return KE_ALREADY_STARTED;
            return start_module(&modules[i], path, args, args_len, modres);
        }
    return KE_UNKNOWN_MODULE;
}

/* Module queries (modload GetModuleIdList and friends). */
static int name_equal(const char *a, const char *b) {
    unsigned i;
    for (i = 0; i < 32; ++i) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}
int ob_iop_module_ids(const char *name, int *ids, int capacity, int *total) {
    unsigned i;
    int stored = 0, count = 0;
    for (i = 0; i < MAX_MODULES; ++i) {
        if (!modules[i].id || (name && !name_equal(modules[i].name, name))) continue;
        if (ids && stored < capacity) ids[stored++] = modules[i].id;
        ++count;
    }
    if (total) *total = count;
    return stored;
}
int ob_iop_module_status(int id, struct ob_module_status *out) {
    unsigned i;
    for (i = 0; i < MAX_MODULES; ++i) {
        const struct module *m = &modules[i];
        if (!m->id || m->id != id) continue;
        ob_zero(out, sizeof(*out));
        ob_copy(out->name, m->name, sizeof(m->name));
        out->version = m->version;
        out->flags = m->started ? 3 : 1; /* loaded, plus running once started */
        out->id = m->id;
        out->entry_addr = m->entry;
        out->gp_value = m->gp;
        out->text_addr = m->base;
        out->text_size = m->text_size;
        out->data_size = m->data_size;
        out->bss_size = m->bss_size;
        return KE_OK;
    }
    return KE_UNKNOWN_MODULE;
}
int ob_iop_module_search(const char *name, uint32_t address) {
    unsigned i;
    int found = KE_UNKNOWN_MODULE;
    address &= 0x1fffffu;
    for (i = 0; i < MAX_MODULES; ++i) {
        const struct module *m = &modules[i];
        if (!m->id) continue;
        if (name ? name_equal(m->name, name) :
            (address >= m->base && address < m->base + m->size))
            if (found < 0 || m->id > found) found = m->id; /* latest instance */
    }
    return found;
}
