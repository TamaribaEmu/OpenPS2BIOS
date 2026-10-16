/* SPDX-License-Identifier: MIT */
/* R3000A exception entry. The full context is saved into the running thread's
   obk_context (pointed to by ob_iop_context), the C dispatcher runs on the
   kernel stack and returns the context to resume, which may belong to another
   thread. Offsets follow struct obk_context: r[n] at 4n, hi 128, lo 132,
   epc 136, sr 140, cause 144. */
.set noreorder
.set noat
.text

.globl ob_iop_vector_install
ob_iop_vector_install:
    /* Write "lui k0,hi; ori k0,lo; jr k0; nop" at the 0x80000080 vector. */
    lui $t0, 0xa000
    lui $t1, %hi(ob_iop_exception_entry)
    addiu $t1, $t1, %lo(ob_iop_exception_entry)
    srl $t2, $t1, 16
    lui $t3, 0x3c1a          /* lui k0, imm */
    or $t3, $t3, $t2
    sw $t3, 0x80($t0)
    andi $t2, $t1, 0xffff
    lui $t3, 0x375a          /* ori k0, k0, imm */
    or $t3, $t3, $t2
    sw $t3, 0x84($t0)
    lui $t3, 0x0340
    ori $t3, $t3, 0x0008     /* jr k0 */
    sw $t3, 0x88($t0)
    jr $ra
    sw $zero, 0x8c($t0)

.globl ob_iop_exception_entry
ob_iop_exception_entry:
    lui $k0, %hi(ob_iop_context)
    lw $k0, %lo(ob_iop_context)($k0)
    nop
    .irp reg,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    sw $\reg, (\reg * 4)($k0)
    .endr
    mfhi $k1
    sw $k1, 128($k0)
    mflo $k1
    sw $k1, 132($k0)
    mfc0 $k1, $14
    nop
    sw $k1, 136($k0)
    mfc0 $k1, $12
    nop
    sw $k1, 140($k0)
    mfc0 $k1, $13
    nop
    sw $k1, 144($k0)
    lui $sp, %hi(ob_iop_kernel_stack_top)
    addiu $sp, $sp, %lo(ob_iop_kernel_stack_top)
    jal ob_iop_exception
    addu $a0, $k0, $zero
    addu $k0, $v0, $zero
    lw $k1, 128($k0)
    nop
    mthi $k1
    lw $k1, 132($k0)
    nop
    mtlo $k1
    lw $k1, 140($k0)
    nop
    mtc0 $k1, $12
    .irp reg,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    lw $\reg, (\reg * 4)($k0)
    .endr
    lw $k1, 136($k0)
    nop
    jr $k1
    .word 0x42000010 /* rfe */

/* int32_t ob_iop_yield(void): trap into the scheduler. Returns the value the
   kernel stored in v0 while this thread waited. */
.globl ob_iop_yield
ob_iop_yield:
    syscall
    jr $ra
    nop

/* A thread whose entry function returns lands here and exits. */
.globl ob_iop_thread_return
ob_iop_thread_return:
    jal ob_iop_exit_thread
    nop
1:  b 1b
    nop

/* sysclib setjmp/longjmp. The buffer holds ra, sp, fp, s0-s7 and gp. */
.globl ob_iop_setjmp
ob_iop_setjmp:
    sw $ra, 0($a0)
    sw $sp, 4($a0)
    sw $fp, 8($a0)
    sw $s0, 12($a0)
    sw $s1, 16($a0)
    sw $s2, 20($a0)
    sw $s3, 24($a0)
    sw $s4, 28($a0)
    sw $s5, 32($a0)
    sw $s6, 36($a0)
    sw $s7, 40($a0)
    sw $gp, 44($a0)
    jr $ra
    addu $v0, $zero, $zero

.globl ob_iop_longjmp
ob_iop_longjmp:
    lw $ra, 0($a0)
    lw $sp, 4($a0)
    lw $fp, 8($a0)
    lw $s0, 12($a0)
    lw $s1, 16($a0)
    lw $s2, 20($a0)
    lw $s3, 24($a0)
    lw $s4, 28($a0)
    lw $s5, 32($a0)
    lw $s6, 36($a0)
    lw $s7, 40($a0)
    lw $gp, 44($a0)
    addu $v0, $a1, $zero
    bne $v0, $zero, 1f
    nop
    addiu $v0, $zero, 1 /* longjmp(buf, 0) makes setjmp return 1 */
1:  jr $ra
    nop

/* int ob_iop_call_on_stack(int (*fn)(void *), void *arg, void *stack_top):
   run fn(arg) on another stack, for deep call chains on threads with small
   stacks. Exceptions save sp in the thread context, so the
   thread may block or be preempted meanwhile. */
.globl ob_iop_call_on_stack
ob_iop_call_on_stack:
    addu $t0, $sp, $zero
    addiu $sp, $a2, -16
    sw $t0, 0($sp)
    sw $ra, 4($sp)
    addu $t9, $a0, $zero
    jalr $t9
    addu $a0, $a1, $zero
    lw $ra, 4($sp)
    lw $t0, 0($sp)
    jr $ra
    addu $sp, $t0, $zero
