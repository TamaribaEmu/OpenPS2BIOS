/* SPDX-License-Identifier: MIT */
.set noreorder
.set noat
.section .text.start,"ax",@progbits
.globl _start
_start:
    /* Reset enters through the uncached alias; continue at the link address. */
    lui $t0, %hi(1f)
    addiu $t0, $t0, %lo(1f)
    jr $t0
    nop
1:  lui $sp, 0xa008
    addiu $sp, $sp, -32
    lui $t0, %hi(__bss_start)
    addiu $t0, $t0, %lo(__bss_start)
    lui $t1, %hi(__bss_end)
    addiu $t1, $t1, %lo(__bss_end)
4:  beq $t0, $t1, 2f
    nop
    sb $zero, 0($t0)
    b 4b
    addiu $t0, $t0, 1
2:  jal ob_iop_main
    nop
3:  b 3b
    nop

/* int32_t ob_iop_call_module(entry, gp, argc, argv): run a module entry point
   with its own $gp; ours is preserved for the caller. */
.text
.globl ob_iop_call_module
ob_iop_call_module:
    addiu $sp, $sp, -32
    sw $ra, 20($sp)
    sw $gp, 16($sp)
    addu $t9, $a0, $zero
    beq $a1, $zero, 1f
    nop
    addu $gp, $a1, $zero
1:  addu $a0, $a2, $zero
    jalr $t9
    addu $a1, $a3, $zero
    lw $gp, 16($sp)
    lw $ra, 20($sp)
    jr $ra
    addiu $sp, $sp, 32
