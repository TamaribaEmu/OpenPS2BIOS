/* SPDX-License-Identifier: MIT */
.set noreorder
.set noat
.section .text.start,"ax",@progbits
.globl _start
_start:
    lui $t0, 0x7000
    mtc0 $t0, $12
    nop
    nop
    lui $sp, 0xa008
    addiu $sp, $sp, -32
    lui $t0, %hi(__bss_start)
    addiu $t0, $t0, %lo(__bss_start)
    lui $t1, %hi(__bss_end)
    addiu $t1, $t1, %lo(__bss_end)
1:  beq $t0, $t1, 2f
    nop
    sb $zero, 0($t0)
    b 1b
    addiu $t0, $t0, 1
2:  jal ob_ee_main
    nop
3:  b 3b
    nop
