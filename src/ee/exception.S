/* SPDX-License-Identifier: MIT */
.set mips3
.set noreorder
.set noat
.text
.globl ob_ee_exception_entry
ob_ee_exception_entry:
    lui $k0, %hi(ob_ee_exception_depth)
    lw $k1, %lo(ob_ee_exception_depth)($k0)
    addiu $k1, $k1, 1
    sw $k1, %lo(ob_ee_exception_depth)($k0)
    sltiu $k0, $k1, 5
    beq $k0, $zero, ob_ee_trap_overflow
    addiu $k1, $k1, -1
    sll $k1, $k1, 10
    lui $k0, %hi(ob_ee_trap_frames)
    addiu $k0, $k0, %lo(ob_ee_trap_frames)
    addu $k0, $k0, $k1
    /* LLVM does not assemble R5900 LQ/SQ; encode those two instructions. */
    .irp reg,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    .word (0x7c000000 | (26 << 21) | (\reg << 16) | (\reg * 16))
    .endr
    mfhi $k1
    sd $k1, 512($k0)
    mflo $k1
    sd $k1, 520($k0)
    mfc0 $k1, $14
    sw $k1, 528($k0)
    mfc0 $k1, $12
    sw $k1, 532($k0)
    mfc0 $k1, $13
    sw $k1, 536($k0)
    .word (0x70000010 | (27 << 11)) /* mfhi1 k1 */
    sd $k1, 544($k0)
    .word (0x70000012 | (27 << 11)) /* mflo1 k1 */
    sd $k1, 552($k0)
    .word ((27 << 11) | 0x28) /* mfsa k1 */
    sw $k1, 560($k0)
    /* Kernel C code never touches the FPU, so a syscall only marks the frame
       (word 540) and ob_ee_fpu_flush saves it lazily. Other exceptions save
       it now. */
    mfc0 $k1, $13
    andi $k1, $k1, 0x7c
    xori $k1, $k1, (8 << 2)
    bne $k1, $zero, 3f
    addiu $k1, $zero, 1
    sw $k1, 540($k0)
    lui $k1, %hi(ob_ee_fpu_pending)
    b 4f
    sw $k0, %lo(ob_ee_fpu_pending)($k1)
3:  sw $zero, 540($k0)
    addu $a0, $k0, $zero
    jal ob_ee_fpu_store
    nop
4:  addu $a0, $k0, $zero
    lui $k1, %hi(ob_ee_exception_depth)
    lw $k1, %lo(ob_ee_exception_depth)($k1)
    addiu $k1, $k1, -1
    sll $k1, $k1, 15
    /* Each exception level gets a 32 KiB stack below 0x78000; programs own
       0x80000 and up. */
    lui $sp, 0xa007
    ori $sp, $sp, 0x8000
    subu $sp, $sp, $k1
    addiu $sp, $sp, -32
    jal ob_ee_exception
    nop
.globl ob_ee_exception_return
ob_ee_exception_return:
    addu $k0, $v0, $zero
    lui $k1, %hi(ob_ee_exception_depth)
    lw $at, %lo(ob_ee_exception_depth)($k1)
    addiu $at, $at, -1
    sw $at, %lo(ob_ee_exception_depth)($k1)
    lw $k1, 528($k0)
    mtc0 $k1, $14
    ld $k1, 512($k0)
    mthi $k1
    ld $k1, 520($k0)
    mtlo $k1
    ld $k1, 544($k0)
    .word (0x70000011 | (27 << 21)) /* mthi1 k1 */
    ld $k1, 552($k0)
    .word (0x70000013 | (27 << 21)) /* mtlo1 k1 */
    lw $k1, 560($k0)
    .word ((27 << 21) | 0x29) /* mtsa k1 */
    lw $k1, 540($k0)
    bne $k1, $zero, 5f       /* FPU still live from the syscall caller */
    nop
    /* CU1 is needed for the restore even if the resumed thread disabled it. */
    mfc0 $k1, $12
    lui $at, 0x2000
    or $k1, $k1, $at
    mtc0 $k1, $12
    nop
    nop
    lw $k1, 564($k0)
    .word (0x44c00000 | (27 << 16) | (31 << 11)) /* ctc1 k1,fcr31 */
    .irp reg,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .word (0xc4000000 | (26 << 21) | (\reg << 16) | (568 + \reg * 4)) /* lwc1 */
    .endr
5:  lw $k1, 532($k0)
    mtc0 $k1, $12
    .irp reg,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    .word (0x78000000 | (26 << 21) | (\reg << 16) | (\reg * 16))
    .endr
    eret
    nop

ob_ee_trap_overflow:
    lui $k0, 0xb000
    ori $k0, $k0, 0xf180
    lui $k1, %hi(ob_ee_depth_message)
    addiu $k1, $k1, %lo(ob_ee_depth_message)
1:  lbu $at, 0($k1)
    beq $at, $zero, 2f
    nop
    sb $at, 0($k0)
    b 1b
    addiu $k1, $k1, 1
2:  b 2b
    nop

/* ob_ee_fpu_store(frame): FPU registers and FCR31 into the frame. Uses only
   k1/at besides a0, so the exception entry may call it before saving. */
.globl ob_ee_fpu_store
ob_ee_fpu_store:
    mfc0 $k1, $12
    lui $at, 0x2000
    or $k1, $k1, $at
    mtc0 $k1, $12
    nop
    nop
    .word (0x44400000 | (27 << 16) | (31 << 11)) /* cfc1 k1,fcr31 */
    sw $k1, 564($a0)
    .irp reg,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .word (0xe4000000 | (4 << 21) | (\reg << 16) | (568 + \reg * 4)) /* swc1 */
    .endr
    jr $ra
    nop

.globl ob_ee_resume_intr_entry
ob_ee_resume_intr_entry:
    jal ob_ee_resume_interrupt
    nop
    j ob_ee_exception_return
    nop

.globl ob_ee_idle
ob_ee_idle:
1:  b 1b
    nop

.globl ob_ee_thread_return
ob_ee_thread_return:
    ori $v1, $zero, 0x23
    syscall
1:  b 1b
    nop

.globl ob_ee_call_custom
ob_ee_call_custom:
    /* o32 parameters: guest gp, handler, a0, a1, a2, a3. Guest code uses
       lq/sq on stack slots, so realign the 8-byte aligned kernel stack to 16.
       Keep every callee-saved register ourselves: some handlers return
       with fp clobbered. */
    addiu $sp, $sp, -96
    sd $s0, 0($sp)
    sd $s1, 8($sp)
    sd $s2, 16($sp)
    sd $s3, 24($sp)
    sd $s4, 32($sp)
    sd $s5, 40($sp)
    sd $s6, 48($sp)
    sd $s7, 56($sp)
    sd $fp, 64($sp)
    sd $gp, 72($sp)
    sd $ra, 80($sp)
    lw $t0, 112($sp)
    lw $t1, 116($sp)
    addu $gp, $a0, $zero
    addu $t9, $a1, $zero
    addu $a0, $a2, $zero
    addu $a1, $a3, $zero
    addu $a2, $t0, $zero
    addu $a3, $t1, $zero
    /* The old sp is kept just above the guest's argument area. */
    addiu $t8, $zero, -16
    and $t8, $sp, $t8
    sw $sp, -8($t8)
    jalr $t9
    addiu $sp, $t8, -48
    sll $v0, $v0, 0 /* sign-extend the 32-bit result; callers test its sign */
    lw $sp, 40($sp)
    ld $s0, 0($sp)
    ld $s1, 8($sp)
    ld $s2, 16($sp)
    ld $s3, 24($sp)
    ld $s4, 32($sp)
    ld $s5, 40($sp)
    ld $s6, 48($sp)
    ld $s7, 56($sp)
    ld $fp, 64($sp)
    ld $gp, 72($sp)
    ld $ra, 80($sp)
    jr $ra
    addiu $sp, $sp, 96

.globl ob_ee_call_custom_regs
ob_ee_call_custom_regs:
    /* Guest gp, handler, frame GPR array. Argument and saved registers come
       from the caller's frame: kernel patches may jump straight back into
       code that was interrupted (Over the Hedge's alarm callbacks return
       through syscall 8 that way) and that code expects its s registers. */
    addiu $sp, $sp, -96
    sd $s0, 0($sp)
    sd $s1, 8($sp)
    sd $s2, 16($sp)
    sd $s3, 24($sp)
    sd $s4, 32($sp)
    sd $s5, 40($sp)
    sd $s6, 48($sp)
    sd $s7, 56($sp)
    sd $fp, 64($sp)
    sd $gp, 72($sp)
    sd $ra, 80($sp)
    addu $gp, $a0, $zero
    addu $t9, $a1, $zero
    addu $v1, $a2, $zero
    /* 16-byte aligned guest stack; the old sp is kept just above its
       argument area. */
    addiu $t8, $zero, -16
    and $t8, $sp, $t8
    sw $sp, -8($t8)
    addiu $t8, $t8, -48
    ld $a0, 4*16($v1)
    ld $a1, 5*16($v1)
    ld $a2, 6*16($v1)
    ld $a3, 7*16($v1)
    ld $t0, 8*16($v1)
    ld $t1, 9*16($v1)
    ld $t2, 10*16($v1)
    ld $t3, 11*16($v1)
    ld $s0, 16*16($v1)
    ld $s1, 17*16($v1)
    ld $s2, 18*16($v1)
    ld $s3, 19*16($v1)
    ld $s4, 20*16($v1)
    ld $s5, 21*16($v1)
    ld $s6, 22*16($v1)
    ld $s7, 23*16($v1)
    ld $fp, 30*16($v1)
    jalr $t9
    addu $sp, $t8, $zero
    lw $sp, 40($sp)
    ld $s0, 0($sp)
    ld $s1, 8($sp)
    ld $s2, 16($sp)
    ld $s3, 24($sp)
    ld $s4, 32($sp)
    ld $s5, 40($sp)
    ld $s6, 48($sp)
    ld $s7, 56($sp)
    ld $fp, 64($sp)
    ld $gp, 72($sp)
    ld $ra, 80($sp)
    jr $ra
    addiu $sp, $sp, 96

.globl ob_ee_call_custom_stack
ob_ee_call_custom_stack:
    /* Seventh o32 argument is the guest stack top. Saved registers as in
       ob_ee_call_custom. */
    addiu $sp, $sp, -96
    sd $s0, 0($sp)
    sd $s1, 8($sp)
    sd $s2, 16($sp)
    sd $s3, 24($sp)
    sd $s4, 32($sp)
    sd $s5, 40($sp)
    sd $s6, 48($sp)
    sd $s7, 56($sp)
    sd $fp, 64($sp)
    sd $gp, 72($sp)
    sd $ra, 80($sp)
    lw $t0, 112($sp)
    lw $t1, 116($sp)
    lw $t2, 120($sp)
    addu $gp, $a0, $zero
    addu $t9, $a1, $zero
    addu $a0, $a2, $zero
    addu $a1, $a3, $zero
    addu $a2, $t0, $zero
    addu $a3, $t1, $zero
    /* Reserve the o32 argument home area below the caller's live frame,
       with the old sp just above it. */
    addiu $t8, $zero, -16
    and $t2, $t2, $t8
    sw $sp, -8($t2)
    jalr $t9
    addiu $sp, $t2, -48
    sll $v0, $v0, 0
    lw $sp, 40($sp)
    ld $s0, 0($sp)
    ld $s1, 8($sp)
    ld $s2, 16($sp)
    ld $s3, 24($sp)
    ld $s4, 32($sp)
    ld $s5, 40($sp)
    ld $s6, 48($sp)
    ld $s7, 56($sp)
    ld $fp, 64($sp)
    ld $gp, 72($sp)
    ld $ra, 80($sp)
    jr $ra
    addiu $sp, $sp, 96

/* ob_ee_tlb_write(index, page_mask, entry_hi, entry_lo0, entry_lo1 on the stack). */
.globl ob_ee_tlb_write
ob_ee_tlb_write:
    lw $t0, 16($sp)
    mtc0 $a0, $0
    mtc0 $a1, $5
    mtc0 $a2, $10
    mtc0 $a3, $2
    mtc0 $t0, $3
    .word 0x0000040f /* sync.p */
    tlbwi
    .word 0x0000040f /* sync.p */
    jr $ra
    nop

/* ob_ee_tlb_read(index, uint32_t out[4]): page mask, hi, lo0, lo1. */
.globl ob_ee_tlb_read
ob_ee_tlb_read:
    mtc0 $a0, $0
    .word 0x0000040f /* sync.p */
    tlbr
    .word 0x0000040f /* sync.p */
    mfc0 $t0, $5
    sw $t0, 0($a1)
    mfc0 $t0, $10
    sw $t0, 4($a1)
    mfc0 $t0, $2
    sw $t0, 8($a1)
    mfc0 $t0, $3
    sw $t0, 12($a1)
    jr $ra
    nop

/* ob_ee_tlb_probe(entry_hi): matching index, or negative when absent. */
.globl ob_ee_tlb_probe
ob_ee_tlb_probe:
    mtc0 $a0, $10
    .word 0x0000040f /* sync.p */
    tlbp
    .word 0x0000040f /* sync.p */
    mfc0 $v0, $0
    jr $ra
    nop

.globl ob_ee_set_wired
ob_ee_set_wired:
    mtc0 $a0, $6
    .word 0x0000040f /* sync.p */
    jr $ra
    nop

.section .rodata
ob_ee_depth_message: .asciz "[openbios:ee] fatal exception: exception depth exceeded\n"
