/* CPU power collapse. cpu_suspend(flag) saves the state the core loses
 * when its power is cut, cleans the caches and asks the secure world to
 * collapse the core (flag 0: L2 stays on, 1: L2 off). It returns nonzero
 * if the secure world bailed out because an interrupt was pending. After
 * a real collapse the secure world restarts the core at cpu_resume with
 * the MMU off; that path restores the state and returns 0 from
 * cpu_suspend. */
        .syntax unified
        .arm
        .fpu    neon

#define SCM_TERMINATE_PC        0x00402221

        .text
        .global cpu_suspend
cpu_suspend:
        push    {r4-r11, lr}
        vpush   {d8-d15}
        vmrs    r1, fpscr
        push    {r0, r1}
        ldr     r1, =suspend_ctx
        mrc     p15, 0, r2, c1, c0, 0           /* SCTLR */
        mrc     p15, 0, r3, c2, c0, 0           /* TTBR0 */
        mrc     p15, 0, r4, c2, c0, 2           /* TTBCR */
        mrc     p15, 0, r5, c3, c0, 0           /* DACR */
        mrc     p15, 0, r6, c12, c0, 0          /* VBAR */
        mrc     p15, 0, r7, c1, c0, 2           /* CPACR */
        vmrs    r8, fpexc
        mrc     p15, 0, r9, c10, c2, 0          /* PRRR */
        mrc     p15, 0, r10, c10, c2, 1         /* NMRR */
        mov     r11, sp
        stmia   r1, {r2-r11}
        bl      dcache_clean_invalidate_all
        ldr     r2, [sp]                        /* flag */
        ldr     r0, =SCM_TERMINATE_PC
        ldr     r1, =scm_ctx
        smc     #0
        mov     r0, #1                          /* bailout */
        b       suspend_return

        .global cpu_resume
cpu_resume:
        cpsid   if, #0x13
        mov     r0, #0
        mcr     p15, 0, r0, c7, c5, 0           /* ICIALLU */
        mcr     p15, 0, r0, c7, c5, 6           /* BPIALL */
        mcr     p15, 0, r0, c8, c7, 0           /* TLBIALL */
        dsb
        isb
        mrc     p15, 0, r0, c1, c0, 1
        orr     r0, r0, #(1 << 6)               /* ACTLR.SMP */
        mcr     p15, 0, r0, c1, c0, 1
        cps     #0x12
        ldr     sp, =__stack_irq_top
        cps     #0x17
        ldr     sp, =__stack_abt_top
        cps     #0x1b
        ldr     sp, =__stack_und_top
        cps     #0x11
        ldr     sp, =__stack_fiq_top
        cps     #0x13
        ldr     r1, =suspend_ctx
        ldmia   r1, {r2-r11}
        mcr     p15, 0, r3, c2, c0, 0
        mcr     p15, 0, r4, c2, c0, 2
        mcr     p15, 0, r5, c3, c0, 0
        mcr     p15, 0, r6, c12, c0, 0
        mcr     p15, 0, r7, c1, c0, 2
        isb
        vmsr    fpexc, r8
        mcr     p15, 0, r9, c10, c2, 0
        mcr     p15, 0, r10, c10, c2, 1
        mov     sp, r11
        dsb
        isb
        mcr     p15, 0, r2, c1, c0, 0           /* MMU and caches back on */
        isb
        mov     r0, #0
suspend_return:
        pop     {r1, r2}
        vmsr    fpscr, r2
        vpop    {d8-d15}
        pop     {r4-r11, pc}

        .bss
        .align  2
suspend_ctx:
        .space  40
scm_ctx:
        .space  4
