/* SPDX-License-Identifier: MIT */
#include "openbios/deci2.h"
#include "openbios/ee.h"
#include "openbios/platform.h"

static struct ob_deci2 service;
extern int32_t ob_ee_call_custom_stack(uint32_t gp, uint32_t handler,
    uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t stack);
static void *resolve(void *ctx, uint32_t address, uint32_t size) {
    (void)ctx;
    return ob_ee_user_pointer(address, size);
}
static void invoke(void *ctx, uint32_t callback, uint32_t gp,
                    int event, int param, uint32_t opt) {
    (void)ctx;
    uint32_t stack = ob_ee_trap_frames[ob_ee_exception_depth - 1].frame.gpr[29][0];
    uint32_t saved = ob_ee_irq_disable();
    ob_ee_fpu_flush();
    ob_ee_irq_restore(saved & ~0x10003u);
    ob_ee_call_custom_stack(gp, callback, (uint32_t)event, (uint32_t)param, opt, 0, stack);
    ob_ee_irq_restore(saved);
}
static int output(void *ctx, const uint8_t *bytes, uint32_t size) {
    volatile uint8_t *sio = (void *)0xb000f180u;
    uint32_t i;
    (void)ctx;
    for (i = 0; i < size; ++i) *sio = bytes[i];
    return 0;
}
void ob_ee_deci2_init(void) {
    static const struct ob_deci2_ops ops = {resolve, invoke, output};
    ob_deci2_init(&service, &ops, 0);
#ifndef OB_DECI2_LOCAL_TTY
    service.no_route = 1; /* no debugger attached */
    service.next_id = 4;  /* the kernel holds sockets 1-4 */
#endif
}
int32_t ob_ee_deci2_call(int32_t command, uint32_t args, uint32_t gp) {
    /* Poll callbacks run on the caller's guest stack, so their local request
       blocks remain ordinary guest pointers during nested syscalls. */
    if (command == 4) {
        uint32_t stack = ob_ee_trap_frames[ob_ee_exception_depth - 1].frame.gpr[29][0];
        if ((stack & 15) || stack < 4096 || !ob_ee_user_pointer(stack - 4096, 4096))
            return OB_D2_INVALADDR;
    }
    int32_t result = ob_deci2_call(&service, command, args, gp);
    if (command == 1) {
        static unsigned reported;
        const uint32_t *a = ob_ee_user_pointer(args, 4);
        if (result > 0 || reported++ < 4) {
            ob_puts("[openbios:deci2] open protocol="); ob_puthex(a ? a[0] & 0xffff : 0);
            ob_puts(" -> "); ob_puthex((uint32_t)result); ob_puts("\n");
        }
    }
    return result;
}
