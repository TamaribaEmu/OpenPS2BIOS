/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_IOP_H
#define OPENBIOS_IOP_H
#include "irx.h"

/* Public IOP kernel error codes (ps2sdk kerr.h values). */
#define KE_OK 0
#define KE_ERROR (-1)
#define KE_LINKERR (-200)
#define KE_ILLEGAL_OBJECT (-201)
#define KE_UNKNOWN_MODULE (-202)
#define KE_NOFILE (-203)
#define KE_FILEERR (-204)
#define KE_ALREADY_STARTED (-206)
#define KE_NOT_STARTED (-208)
#define KE_CAN_NOT_STOP (-209)
#define KE_NOT_STOPPED (-210)
#define KE_NOT_REMOVABLE (-211)
#define KE_LIBRARY_FOUND (-212)
#define KE_LIBRARY_NOTFOUND (-213)
#define KE_ILLEGAL_LIBRARY (-214)
#define KE_LIBRARY_INUSE (-215)
#define KE_NO_MEMORY (-400)
#define KE_ILLEGAL_MEMBLOCK (-426)

/* Module start results. */
#define OB_MODULE_RESIDENT_END 0
#define OB_MODULE_NO_RESIDENT_END 1
#define OB_MODULE_REMOVABLE_END 2

/* sysmem: memory available to modules and the IOP heap. */
/* Module memory runs from the end of this firmware's image to OB_SYSMEM_END,
   except for the reserved window holding the boot stack and the buffers
   shared with the EE (OB_SYSMEM_HOLE_START up to OB_SYSMEM_HOLE_END). */
#define OB_SYSMEM_HOLE_START 0x00078000u
#define OB_SYSMEM_HOLE_END 0x00089000u
#define OB_SYSMEM_END 0x001fff00u
uint32_t ob_sysmem_base(void);
#define OB_ALLOC_FIRST 0
#define OB_ALLOC_LAST 1
#define OB_ALLOC_ADDRESS 2
void ob_sysmem_init(void);
void *ob_sysmem_alloc(int type, uint32_t size, uint32_t address);
int ob_sysmem_free(void *block);
uint32_t ob_sysmem_free_size(void);
uint32_t ob_sysmem_largest_free(void);
uint32_t ob_sysmem_block(uint32_t address, uint32_t *size);
void ob_sysmem_dump(void);

/* loadcore */
void ob_loadcore_init(void);
int ob_iop_register_library(struct ob_irx_exports *exports);
int ob_iop_release_library(struct ob_irx_exports *exports);
/* Load and link without starting; start later with ob_iop_start_module. */
int ob_iop_load_module_only(const void *file, uint32_t size, const char *path);
/* As ob_iop_load_module_only, into caller-provided memory at address. */
int ob_iop_load_module_at(const void *file, uint32_t size, const char *path, uint32_t address);
int ob_iop_start_module(int id, const char *path, const char *args, uint32_t args_len, int *modres);
int ob_iop_stop_module(int id, const char *args, uint32_t args_len, int *modres);
int ob_iop_unload_module(int id);
void ob_iop_modload_init(void);
/* Load, link and start a module image. Returns the module ID (> 0) or a KE_
   error; *modres receives the start result. */
int ob_iop_load_module(const void *file, uint32_t size, const char *path,
                       const char *args, uint32_t args_len, int *modres);
/* Public modload ModuleStatus layout. */
struct ob_module_status {
    char name[56];
    uint16_t version, flags;
    int id;
    uint32_t entry_addr, gp_value, text_addr, text_size, data_size, bss_size, reserved[2];
};
int ob_iop_module_ids(const char *name, int *ids, int capacity, int *total);
int ob_iop_module_status(int id, struct ob_module_status *out);
/* By name, or by an address inside the module when name is NULL. */
int ob_iop_module_search(const char *name, uint32_t address);
int32_t ob_iop_call_module(uint32_t entry, uint32_t gp, int argc, char **argv);
void ob_iop_libraries_init(void);
void ob_iop_kernel_init(void);
void ob_iop_kernel_libraries_init(void);
void ob_iop_kernel_reset(void);
void ob_iop_dmacman_init(void);
void ob_iop_cdvdman_init(void);
void ob_iop_secrman_init(void);
void ob_iop_timrman_init(void);
int ob_intr_register(int irq, int (*handler)(void *), void *arg);
void ob_intr_release(int irq);
void ob_intr_enable(int irq, int enable);
int ob_intr_context(void);
int ob_kernel_wakeup(int id);
int ob_kernel_sleep(void);
int ob_kernel_current_thread(void);
/* Block the calling thread for about usec when it may block (thread context,
   interrupts enabled); otherwise return immediately. Returns 1 if it blocked. */
int ob_kernel_pause(uint32_t usec);
/* Change the calling thread's priority; returns the previous one. */
int ob_kernel_set_priority(int priority);
/* Create and start a kernel-owned thread; returns its ID or an error. */
int ob_kernel_spawn(void (*entry)(void *), void *arg, int priority, uint32_t stack_size);
/* Send a SIF command packet (16-byte header first), transferring size bytes
   from src to EE dest beforehand; returns 0 on failure. */
uint32_t ob_iop_send_cmd(uint32_t cid, void *packet, int psize, void *src, uint32_t dest, int size);
/* Copy EE memory to the IOP through an RPC "other data" request (waits). */
int ob_iop_get_other_data(uint32_t ee_src, void *dest, int size);
void ob_iop_cd_servers_init(void);
/* Serve RPC ID sid (sceSifRpcLoop semantics): func receives the request in
   buf and returns the reply data. */
int ob_iop_rpc_server_join(int sid, void *(*func)(int fno, void *buf, int size), void *buf, int with);
int ob_iop_rpc_server_start(int sid, void *(*func)(int fno, void *buf, int size), void *buf,
                            int priority, uint32_t stack_size);
/* From a server started with ob_iop_rpc_server_start: send the reply to the
   request being served now, before func returns (its return value is then
   ignored). The EE may send the next request into buf right away. */
int ob_iop_rpc_reply_early(void *result);
/* Priority of the built-in service thread (thread 1), which receives SIF
   commands and runs the built-in RPC servers. Commands are normally handled
   in interrupt context, so module threads must not starve it. */
#define OB_IOP_SERVICE_PRIORITY 16
/* Module entry points run at modload's loader priority, not LOADFILE's RPC
   priority (88). Startup may create workers before initializing their semaphores;
   those workers must not preempt it (GT4 INET creates a priority-48 worker).
   Public reference: ps2sdk iop/system/modload/src/modload.c, ExecModuleLoad. */
#define OB_IOP_MODULE_START_PRIORITY 8
/* Kernel tick hook: wakes the service thread while the boot mailbox has work. */
void ob_iop_disc_tick(void);
void ob_iop_sifrpc_init(void);
void ob_iop_fileio_init(void);
int ob_cd_scommand(uint8_t cmd, const void *params, unsigned count, void *result, unsigned want);
uint64_t ob_iop_system_time(void);
/* LOADFILE GET_VERSION reply, set from the IOPRP image named at reboot. */
extern char ob_iop_loadfile_version[4];
#endif
