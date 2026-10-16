/* SPDX-License-Identifier: MIT */
.set noreorder
.set noat
.section .text.start,"ax",@progbits
.globl _start
_start:
    /* One MIPS-I reset path, shared by the R5900 and R3000A. */
    mtc0 $zero, $12
    nop
    nop
    mfc0 $t0, $15
    nop
    andi $t0, $t0, 0xff00
    ori $t1, $zero, 0x2e00
    beq $t0, $t1, ee_name
    nop
    /* The IOP image starts at 0x1000, just above its exception vectors, to
       leave as much IOP RAM as possible for modules. The EE kernel starts at
       0x10000. */
    lui $t8, 0xa000
    ori $t8, $t8, 0x1000
    lui $t7, %hi(iop_name)
    b find_module
    addiu $t7, $t7, %lo(iop_name)
ee_name:
    lui $t8, 0xa001
    lui $t7, %hi(ee_module_name)
    addiu $t7, $t7, %lo(ee_module_name)
find_module:
    lui $t0, 0xbfc0
    ori $t0, $t0, 0x1000
    lui $t1, 0xbfc0
    ori $t6, $zero, 32
next_entry:
    lbu $t2, 0($t0)
    nop
    beq $t2, $zero, fail
    nop
    addu $t3, $t0, $zero
    addu $t4, $t7, $zero
compare:
    lbu $t2, 0($t3)
    lbu $t5, 0($t4)
    nop
    bne $t2, $t5, skip
    nop
    beq $t5, $zero, found
    addiu $t3, $t3, 1
    b compare
    addiu $t4, $t4, 1
skip:
    lw $t2, 12($t0)
    nop
    addiu $t2, $t2, 15
    ori $t3, $zero, 15
    nor $t3, $t3, $zero
    and $t2, $t2, $t3
    addu $t1, $t1, $t2
    addiu $t0, $t0, 16
    addiu $t6, $t6, -1
    bne $t6, $zero, next_entry
    nop
fail:
    b fail
    nop
found:
    lw $t2, 12($t0)
    addu $t3, $t8, $zero
    lui $t5, 0x0007
    sltu $t5, $t2, $t5
    beq $t5, $zero, fail
    nop
    beq $t2, $zero, fail
    nop
    lui $t5, 0xbfc0
    ori $t5, $t5, 0x1000
    sltu $t5, $t1, $t5
    bne $t5, $zero, fail
    nop
    addu $t5, $t1, $t2
    lui $t6, 0xc000
    sltu $t6, $t6, $t5
    bne $t6, $zero, fail
    nop
copy:
    lbu $t4, 0($t1)
    addiu $t1, $t1, 1
    sb $t4, 0($t3)
    addiu $t2, $t2, -1
    bne $t2, $zero, copy
    addiu $t3, $t3, 1
    jr $t8
    nop
ee_module_name: .asciz "EELOAD"
iop_name: .asciz "IOPBOOT"
