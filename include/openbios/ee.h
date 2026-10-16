/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_EE_H
#define OPENBIOS_EE_H
#include "base.h"
/* GPR slots preserve all 128 bits, including registers unused by the C ABI. */
struct ob_ee_frame {
    uint32_t gpr[32][4];
    uint64_t hi, lo;
    uint32_t epc, status, cause, reserved;
    uint64_t hi1, lo1;
    uint32_t sa, fcr31, fpu[32];
} __attribute__((aligned(16)));
#define OB_EE_TRAP_LEVELS 4
union ob_ee_trap_slot {
    struct ob_ee_frame frame;
    uint8_t storage[1024];
};
extern union ob_ee_trap_slot ob_ee_trap_frames[OB_EE_TRAP_LEVELS];
extern uint32_t ob_ee_exception_depth;
struct ob_ee_frame *ob_ee_exception(struct ob_ee_frame *frame);
void ob_ee_vectors_init(void);
void ob_ee_tlb_init(void);
int ob_ee_tlb_init_standard(void);
void ob_ee_tlb_write(uint32_t index, uint32_t page_mask, uint32_t hi, uint32_t lo0, uint32_t lo1);
void ob_ee_tlb_read(uint32_t index, uint32_t out[4]);
int ob_ee_tlb_probe(uint32_t hi);
void ob_ee_set_wired(uint32_t count);
void ob_ee_kernel_init(void);
void ob_ee_kernel_strict(int enabled);
void *ob_ee_user_pointer(uint32_t address, uint32_t size);
void ob_ee_deci2_init(void);
void ob_ee_sif_init(void);
void ob_ee_sif_set_dchain(void);
void ob_ee_sif_stop_dma(void);
int32_t ob_ee_sif_get_reg(uint32_t id);
int32_t ob_ee_sif_set_reg(uint32_t id, uint32_t value);
uint32_t ob_ee_sif_set_dma(uint32_t descriptors, int32_t count);
int32_t ob_ee_sif_dma_stat(uint32_t id);
void ob_ee_sif_dma_irq(void);
void ob_ee_sif_dma_mask_changed(void);
int32_t ob_ee_deci2_call(int32_t command, uint32_t args, uint32_t gp);
void ob_ee_interrupts_enable(void);
uint32_t ob_ee_irq_disable(void);
void ob_ee_irq_restore(uint32_t status);
int32_t ob_ee_call_guest(uint32_t gp, uint32_t handler,
                        uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
/* Save FPU state a syscall left live (see exception.S) before guest code runs
   or the running thread can change. */
void ob_ee_fpu_flush(void);
void ob_ee_install_eeload(void);
void ob_ee_run_eeload(void) __attribute__((noreturn));
/* Calls a guest syscall handler with the caller's a0-a3 and t0-t3. */
int32_t ob_ee_call_guest_frame(uint32_t gp, uint32_t handler, const struct ob_ee_frame *f);
void ob_ee_alarm_init(void);
int32_t ob_ee_alarm_set_entry(uint32_t time, uint32_t callback, uint32_t common);
int32_t ob_ee_alarm_release_entry(int32_t id);
void ob_ee_alarm_irq_entry(void);
struct ob_ee_frame *ob_ee_resume_interrupt(void);
#endif
