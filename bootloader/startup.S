/* SPDX-License-Identifier: MIT */
#include "reset_marker.h"
.syntax unified
.cpu cortex-m4
.thumb
.section .vectors,"a",%progbits
.word BOOT_MARKER_ADDR
.word Reset_Handler
.rept 62
.word Default_Handler
.endr

.section .boot_id,"a",%progbits
.word 0x534f5450 /* BOOT_ID_MAGIC */
.word 2 /* SRAM marker/linker contract */
.word TOTP_BOOT_UNLOCKED
.word 0xf4000

.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
.thumb_func
Reset_Handler:
	cpsid i
	ldr r0,=0x40000400
	ldr r4,[r0]
	movs r5,#0 /* Only a validated public stage may survive the wipe. */
#if TOTP_BOOT_UNLOCKED
	mov.w r10,#0
#endif
	ldr r0,=BOOT_MARKER_ADDR
	ldmia r0,{r6-r9}
	ldr r0,=BOOT_MARKER_MAGIC
	cmp r6,r0
	bne .Lbench_marker
	mvn r0,r6
	cmp r9,r0
	bne .Lclear_raw_marker
	mvn r0,r7
	cmp r8,r0
	bne .Lclear_raw_marker
	cmp r4,#1 /* Ignore even a valid marker for every non-pin reset. */
	bne .Lclear_raw_marker
	cmp r7,#BOOT_STAGE_ONE
	beq .Lvalid_stage
	cmp r7,#BOOT_STAGE_TWO
	bne .Lclear_raw_marker
.Lvalid_stage:
	mov r5,r7
	b .Lclear_raw_marker
.Lbench_marker:
#if TOTP_BOOT_UNLOCKED
	ldr r0,=BOOT_BENCH_WIPE_MAGIC
	cmp r6,r0
	bne .Lclear_raw_marker
	mvn r0,r6
	cmp r9,r0
	bne .Lclear_raw_marker
	cmp r7,#0
	bne .Lclear_raw_marker
	mvn r0,r7
	cmp r8,r0
	bne .Lclear_raw_marker
	mov.w r10,#1
#endif
.Lclear_raw_marker:
	/* Never carry arbitrary old RAM contents into C or the new stack. */
	movs r6,#0
	movs r7,#0
	mov.w r8,#0
	mov.w r9,#0
	ldr r0,=BOOT_RAM_START
	ldr r1,=BOOT_RAM_END
	movs r2,#0
.global boot_wipe_begin
boot_wipe_begin:
	str r2,[r0],#4
	cmp r0,r1
	blo boot_wipe_begin
	dsb
.global boot_wipe_done
boot_wipe_done:
#if TOTP_BOOT_UNLOCKED
	cmp r10,#1
	bne boot_after_wipe
.global boot_wipe_wait
boot_wipe_wait:
	b boot_wipe_wait /* Bench can reconnect after a physical debug reset. */
#endif
.global boot_after_wipe
boot_after_wipe:
	ldr r1,=BOOT_MARKER_ADDR
	msr msp,r1 /* Marker area stays outside the boot stack. */
	ldr r0,=__data_load
	ldr r1,=__data_start
	ldr r2,=__data_end
1:
	cmp r1,r2
	bhs 2f
	ldr r3,[r0],#4
	str r3,[r1],#4
	b 1b
2:
	mov r0,r4
	mov r1,r5
	bl boot_main
	b Default_Handler
.size Reset_Handler,.-Reset_Handler

.section .text.Default_Handler,"ax",%progbits
.global Default_Handler
.type Default_Handler,%function
.thumb_func
Default_Handler:
	cpsid i
	wfe
	b Default_Handler
.size Default_Handler,.-Default_Handler
