/* SPDX-License-Identifier: MIT */
#include "openbios/alarm.h"
#include "openbios/ee.h"

#define T3_COUNT (*(volatile uint32_t *)0xb0001800u)
#define T3_MODE (*(volatile uint32_t *)0xb0001810u)
#define T3_COMP (*(volatile uint32_t *)0xb0001820u)
#define INTC_MASK (*(volatile uint32_t *)0xb000f010u)
#define TIMER_MODE 0x083u /* H-sync clock, count enabled, no interrupts. */
#define TIMER_IRQS 0x300u /* Compare and overflow interrupts. */
#define TIMER_FLAGS 0xc00u
static struct ob_alarm_queue alarms;
static uint16_t previous_count;

uint32_t ob_ee_irq_disable(void) {
    uint32_t status, disabled;
    __asm__ volatile ("mfc0 %0, $12" : "=r"(status));
    disabled = status & ~0x10000u;
    __asm__ volatile ("mtc0 %0, $12\nnop\nnop\nsync" :: "r"(disabled) : "memory");
    return status;
}
void ob_ee_irq_restore(uint32_t status) {
    __asm__ volatile ("sync\nmtc0 %0, $12\nnop\nnop" :: "r"(status) : "memory");
}
void ob_ee_interrupts_enable(void) {
    uint32_t status;
    __asm__ volatile ("mfc0 %0, $12" : "=r"(status));
    ob_ee_irq_restore(status | 0x10c01u); /* IE, EIE, INTC and DMAC masks. */
}
extern int32_t ob_ee_call_custom(uint32_t gp, uint32_t handler,
                                uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern void ob_ee_fpu_store(struct ob_ee_frame *f);
struct ob_ee_frame *ob_ee_fpu_pending;
void ob_ee_fpu_flush(void) {
    struct ob_ee_frame *f = ob_ee_fpu_pending;
    ob_ee_fpu_pending = 0;
    if (f && f->reserved) {
        ob_ee_fpu_store(f);
        f->reserved = 0;
    }
}
int32_t ob_ee_call_guest(uint32_t gp, uint32_t handler,
                        uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t saved = ob_ee_irq_disable(), depth = ob_ee_exception_depth;
    int32_t result;
    ob_ee_fpu_flush();
    /* Clear EXL so a nested SYSCALL captures a fresh EPC, IE so a callback's
       EI cannot nest hardware IRQs, and KSU so kernel code never runs in
       user mode once EXL drops. */
    ob_ee_irq_restore(saved & ~0x1001bu);
    result = ob_ee_call_custom(gp, handler, a0, a1, a2, a3);
    /* Kernel patches may abandon a nested syscall by restoring a saved ra/sp;
       drop any exception levels left behind. */
    ob_ee_exception_depth = depth;
    ob_ee_irq_restore(saved);
    return result;
}
extern int32_t ob_ee_call_custom_regs(uint32_t gp, uint32_t handler, const void *gprs);
int32_t ob_ee_call_guest_frame(uint32_t gp, uint32_t handler, const struct ob_ee_frame *f) {
    uint32_t saved = ob_ee_irq_disable(), depth = ob_ee_exception_depth;
    int32_t result;
    ob_ee_fpu_flush();
    ob_ee_irq_restore(saved & ~0x1001bu); /* As ob_ee_call_guest. */
    result = ob_ee_call_custom_regs(gp, handler, f->gpr);
    ob_ee_exception_depth = depth;
    ob_ee_irq_restore(saved);
    return result;
}
static void update_time(void) {
    uint32_t mode = T3_MODE;
    uint16_t count = (uint16_t)T3_COUNT;
    uint32_t elapsed = (uint16_t)(count - previous_count);
    if ((mode & 0x800) && count >= previous_count) elapsed += 65536;
    previous_count = count;
    ob_alarm_advance(&alarms, elapsed);
    T3_MODE = (mode & ~TIMER_FLAGS) | TIMER_FLAGS;
}
static void rearm(void) {
    int32_t delay = ob_alarm_next_delay(&alarms);
    uint32_t mode = TIMER_MODE;
    if (delay >= 0) {
        uint16_t count;
        if (!delay) delay = 1;
        count = (uint16_t)T3_COUNT;
        if ((uint16_t)(count - previous_count) >= (uint32_t)delay)
            T3_COMP = ((uint32_t)count + 1) & 0xffff;
        else T3_COMP = ((uint32_t)previous_count + (uint32_t)delay) & 0xffff;
        /* Interrupt only while an alarm is pending: replacement TIMER3
           handlers often assume every TIMER3 interrupt is their own. */
        mode |= TIMER_IRQS;
    }
    T3_MODE = mode; /* Zero flag bits preserve flags latched since update_time. */
}
void ob_ee_alarm_init(void) {
    ob_alarm_init(&alarms);
    T3_MODE = TIMER_FLAGS;
    T3_COUNT = 0;
    previous_count = 0;
    T3_COMP = 0xffff;
    T3_MODE = TIMER_MODE;
    if (!(INTC_MASK & (1u << 12))) INTC_MASK = 1u << 12;
}
int32_t ob_ee_alarm_set_entry(uint32_t time, uint32_t callback, uint32_t common) {
    uint32_t gp, status = ob_ee_irq_disable();
    int32_t id;
    __asm__ volatile ("move %0, $gp" : "=r"(gp));
    if (time > 65535 || !callback || (callback & 3) ||
        (callback & 0x1fffffffu) < 0x1000 || (callback & 0x1fffffffu) >= 0x02000000u) {
        ob_ee_irq_restore(status);
        return OB_EINVAL;
    }
    update_time();
    id = ob_alarm_set(&alarms, (uint16_t)time, callback, common, gp);
    rearm();
    ob_ee_irq_restore(status);
    return id;
}
int32_t ob_ee_alarm_release_entry(int32_t id) {
    uint32_t status = ob_ee_irq_disable();
    int32_t result;
    update_time();
    result = ob_alarm_release(&alarms, id);
    rearm();
    ob_ee_irq_restore(status);
    return result;
}
void ob_ee_alarm_irq_entry(void) {
    struct ob_alarm_event event;
    update_time();
    /* Removing before invocation lets callbacks safely set/release alarms. */
    while (ob_alarm_take(&alarms, &event) == 1) {
        ob_ee_call_guest(event.gp, event.callback, (uint32_t)event.id,
                         event.time, event.common, 0);
        update_time();
    }
    rearm();
}
