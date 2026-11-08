/* SPDX-License-Identifier: MIT */
.syntax unified
.thumb
.section .text.boot_mbr_call,"ax",%progbits
.global boot_mbr_call
.type boot_mbr_call,%function
.thumb_func
boot_mbr_call:
	svc #0x18
	bx lr
.size boot_mbr_call,.-boot_mbr_call

.section .text.boot_sd_disable,"ax",%progbits
.global boot_sd_disable
.type boot_sd_disable,%function
.thumb_func
boot_sd_disable:
	svc #0x11
	bx lr
.size boot_sd_disable,.-boot_sd_disable

.section .text.boot_sd_vectors,"ax",%progbits
.global boot_sd_vectors
.type boot_sd_vectors,%function
.thumb_func
boot_sd_vectors:
	svc #0x13
	bx lr
.size boot_sd_vectors,.-boot_sd_vectors

.section .text.boot_jump,"ax",%progbits
.global boot_jump
.type boot_jump,%function
.thumb_func
boot_jump:
	cpsid i
	ldr r1,[r0]
	ldr r2,[r0,#4]
	msr msp,r1
	movs r3,#0
	msr control,r3
	msr basepri,r3
	isb
	mov lr,#-1
	cpsie i
	bx r2
.size boot_jump,.-boot_jump
