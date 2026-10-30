/* SPDX-License-Identifier: MIT */
/* host: calls go through an ioman import table, as from any IRX module, so
   an emulator that serves host: files at ioman's import stubs (PCSX2) sees
   them. Each entry sets -ENODEV first: where nothing serves the stub, it
   just returns. */
.set noreorder
.set noat

.macro HOST name, index
.globl ob_host_\name
ob_host_\name:
    j stub_\name
    addiu $v0, $zero, -19
.endm

.text
HOST open, 4
HOST close, 5
HOST read, 6
HOST write, 7
HOST lseek, 8
HOST remove, 10
HOST mkdir, 11
HOST rmdir, 12
HOST dopen, 13
HOST dclose, 14
HOST dread, 15
HOST getstat, 16

.macro STUB name, index
stub_\name:
    jr $ra
    addiu $zero, $zero, \index
.endm

.balign 4
    .word 0x41e00000 /* import table: magic, next, version 1.01, name */
    .word 0
    .word 0x00000101
    .ascii "ioman\0\0\0"
STUB open, 4
STUB close, 5
STUB read, 6
STUB write, 7
STUB lseek, 8
STUB remove, 10
STUB mkdir, 11
STUB rmdir, 12
STUB dopen, 13
STUB dclose, 14
STUB dread, 15
STUB getstat, 16
    .word 0, 0
