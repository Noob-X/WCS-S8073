/* commit: 6b5f6ab0e1c33beaed828271f13c03ed02ee3c15
 * ARM: pm: make MULTI_CPU and !MULTI_CPU resume paths the same
 * commit: 2fefbcd58590cf33189c6178098e12b31b994b5f
 * ARM: pm: move return address (for cpu_resume) to top of stack
 * commit: 3fd431bd0cbc75a506b90b42619de3b04fe813a7
 * ARM: pm: extract common code from MULTI_CPU/!MULTI_CPU paths
 * commit: 5fa94c812c0001ac7c3d8868e956ec514734a352
 * ARM: pm: preserve r4 - r11 across a suspend
 * commit: 8111eaa6d424ab3ba8a4d7a3148d4681ae5c6ae3
 * ARM: pm: reallocate registers to avoid r2, r3
 * commit: 3799bbe57843d279008c9ec3406838966cad5f15
 * ARM: pm: rejig suspend follow-on function calling convention
 */
#include <linux/linkage.h>
#include <linux/threads.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>
#include <asm/glue-cache.h>
#include <asm/glue-proc.h>
#include <asm/system.h>
	.text

/*
 * Save CPU state for a suspend
 *  r1 = v:p offset
 *  r2 = suspend function arg0
 *  r3 = suspend function
 * Note: does not return until system resumes
 */
ENTRY(cpu_suspend)
	stmfd	sp!, {r4 - r11, lr}
#ifdef MULTI_CPU
	ldr	r10, =processor
	ldr	r5, [r10, #CPU_SLEEP_SIZE] @ size of CPU sleep state
	ldr	ip, [r10, #CPU_DO_RESUME] @ virtual resume function
#else
/*	ldr	r0, =cpu_suspend_size */
	ldr	r5, =cpu_suspend_size
	ldr	ip, =cpu_do_resume
#endif
/*	mov	r2, sp			@ current virtual SP
	sub	sp, sp, r0		@ allocate CPU state on stack */
	mov	r6, sp			@ current virtual SP
	sub	sp, sp, r5		@ allocate CPU state on stack
	mov	r0, sp			@ save pointer
	add	ip, ip, r1		@ convert resume fn to phys
/*	stmfd	sp!, {r1, r2, r3, ip}	@ save v:p, virt SP, retfn, phys resume fn 
	stmfd	sp!, {r1, r2, ip}	@ save v:p, virt SP, phys resume fn
	ldr	r3, =sleep_save_sp
	add	r2, sp, r1		@ convert SP to phys */
	stmfd	sp!, {r1, r6, ip}	@ save v:p, virt SP, phys resume fn
	ldr	r5, =sleep_save_sp
	add	r6, sp, r1		@ convert SP to phys
	stmfd	sp!, {r2, r3}		@ save suspend func arg and pointer
#ifdef CONFIG_SMP
	ALT_SMP(mrc p15, 0, lr, c0, c0, 5)
	ALT_UP(mov lr, #0)
	and	lr, lr, #15
/*	str	r2, [r3, lr, lsl #2]	@ save phys SP */
	str	r6, [r5, lr, lsl #2]	@ save phys SP
#else
/*	str	r2, [r3]		@ save phys SP */
	str	r6, [r5]		@ save phys SP
#endif
#ifdef MULTI_CPU
	mov	lr, pc
	ldr	pc, [r10, #CPU_DO_SUSPEND] @ save CPU state
#else
/*	mov	r2, sp			@ current virtual SP
	ldr	r0, =cpu_suspend_size
	ldr	ip, =cpu_do_resume
	sub	sp, sp, r0		@ allocate CPU state on stack
	mov	r0, sp			@ save pointer
	stmfd	sp!, {r1, r2, r3}	@ save v:p, virt SP, return fn 
	add	ip, ip, r1		@ convert resume fn to phys
	stmfd	sp!, {r1, r2, r3, ip}	@ save v:p, virt SP, retfn, phys resume fn 
	stmfd	sp!, {r1, r2, ip}	@ save v:p, virt SP, phys resume fn
	ldr	r3, =sleep_save_sp
	add	r2, sp, r1		@ convert SP to phys
#ifdef CONFIG_SMP
	ALT_SMP(mrc p15, 0, lr, c0, c0, 5)
	ALT_UP(mov lr, #0)
	and	lr, lr, #15
	str	r2, [r3, lr, lsl #2]	@ save phys SP
#else
	str	r2, [r3]		@ save phys SP
#endif */
	bl	cpu_do_suspend
#endif

	@ flush data cache
#ifdef MULTI_CACHE
	ldr	r10, =cpu_cache
/*	mov	lr, r9 */
	mov	lr, pc
	ldr	pc, [r10, #CACHE_FLUSH_KERN_ALL]
#else
/*	mov	lr, r9
	b	__cpuc_flush_kern_all */
	bl	__cpuc_flush_kern_all
#endif
	ldmfd	sp!, {r0, pc}		@ call suspend fn
ENDPROC(cpu_suspend)
	.ltorg

/*
 * r0 = control register value
 * r1 = v:p offset (preserved by cpu_do_resume)
 * r2 = phys page table base
 * r3 = L1 section flags
 */
ENTRY(cpu_resume_mmu)
	adr	r4, cpu_resume_turn_mmu_on
	mov	r4, r4, lsr #20
	orr	r3, r3, r4, lsl #20
	ldr	r5, [r2, r4, lsl #2]	@ save old mapping
	str	r3, [r2, r4, lsl #2]	@ setup 1:1 mapping for mmu code
	sub	r2, r2, r1
	ldr	r3, =cpu_resume_after_mmu
	bic	r1, r0, #CR_C		@ ensure D-cache is disabled
	b	cpu_resume_turn_mmu_on
ENDPROC(cpu_resume_mmu)
	.ltorg
	.align	5
cpu_resume_turn_mmu_on:
	mcr	p15, 0, r1, c1, c0, 0	@ turn on MMU, I-cache, etc
	mrc	p15, 0, r1, c0, c0, 0	@ read id reg
	mov	r1, r1
	mov	r1, r1
	mov	pc, r3			@ jump to virtual address
ENDPROC(cpu_resume_turn_mmu_on)
cpu_resume_after_mmu:
	str	r5, [r2, r4, lsl #2]	@ restore old mapping
	mcr	p15, 0, r0, c1, c0, 0	@ turn on D-cache
	bl	cpu_init		@ restore the und/abt/irq banked regs
	ldmfd	sp!, {r4 - r11, pc}
ENDPROC(cpu_resume_after_mmu)

/*
 * Note: Yes, part of the following code is located into the .data section.
 *       This is to allow sleep_save_sp to be accessed with a relative load
 *       while we can't rely on any MMU translation.  We could have put
 *       sleep_save_sp in the .text section as well, but some setups might
 *       insist on it to be truly read-only.
 */
	.data
	.align
ENTRY(cpu_resume)
#ifdef CONFIG_SMP
	adr	r0, sleep_save_sp
	ALT_SMP(mrc p15, 0, r1, c0, c0, 5)
	ALT_UP(mov r1, #0)
	and	r1, r1, #15
	ldr	r0, [r0, r1, lsl #2]	@ stack phys addr
#else
	ldr	r0, sleep_save_sp	@ stack phys addr
#endif
	setmode	PSR_I_BIT | PSR_F_BIT | SVC_MODE, r1  @ set SVC, irqs off
/* #ifdef MULTI_CPU */
/*	@ load v:p, stack, return fn, resume fn
  ARM(	ldmia	r0!, {r1, sp, lr, pc}	)
THUMB(	ldmia	r0!, {r1, r2, r3, r4}	) */
	@ load v:p, stack, resume fn
  ARM(	ldmia	r0!, {r1, sp, pc}	)
THUMB(	ldmia	r0!, {r1, r2, r3}	)
THUMB(	mov	sp, r2			)
/* THUMB(	mov	lr, r3			)
THUMB(	bx	r4			) */
THUMB(	bx	r3			)
/* #else
	@ load v:p, stack, return fn
  ARM(	ldmia	r0!, {r1, sp, lr}	)
THUMB(	ldmia	r0!, {r1, r2, lr}	)
THUMB(	mov	sp, r2			)
	b	cpu_do_resume
#endif */
ENDPROC(cpu_resume)

sleep_save_sp:
	.rept	CONFIG_NR_CPUS
	.long	0				@ preserve stack phys ptr here
	.endr
