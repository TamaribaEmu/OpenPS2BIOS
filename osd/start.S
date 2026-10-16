/* SPDX-License-Identifier: MIT */
/* Boot screen program entry and helpers. */
.set noreorder
.set noat
.section .text.start,"ax",@progbits
.globl _start
_start:
    lui $sp, %hi(osd_stack_top)
    addiu $sp, $sp, %lo(osd_stack_top)
    jal osd_main
    nop
1:  b 1b
    nop

.text
/* int osd_syscall(int number, a0, a1, a2, a3): PS2 syscall number in v1. */
.globl osd_syscall
osd_syscall:
    lw $t0, 16($sp)
    addu $v1, $a0, $zero
    addu $a0, $a1, $zero
    addu $a1, $a2, $zero
    addu $a2, $a3, $zero
    addu $a3, $t0, $zero
    syscall
    jr $ra
    nop

/* void osd_write64(address, low, high): one 64-bit store (GS registers). */
.set mips3
.globl osd_write64
osd_write64:
    dsll32 $a2, $a2, 0
    dsll32 $a1, $a1, 0
    dsrl32 $a1, $a1, 0
    or $a2, $a2, $a1
    sd $a2, 0($a0)
    jr $ra
    nop

.bss
.align 4
.space 0x4000
.globl osd_stack_top
osd_stack_top:
