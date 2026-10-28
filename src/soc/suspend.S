/* CPU power collapse. cpu_suspend(flag) saves the state the core loses
 * when its power is cut, cleans the caches and asks the secure world to
 * collapse the core (flag 0: L2 stays on, 1: L2 off). The secure world
 * expects the core to hold the handoff lock, a hardware mutex tagged with
 * 128 + the core number, which it releases itself. It returns nonzero
 * if the secure world bailed out because an interrupt was pending. After
 * a real collapse the secure world restarts the core at cpu_resume with
 * the MMU off; that path restores the state and returns 0 from
 * cpu_suspend. */
        .syntax unified
        .arm
        .fpu    neon

#define SCM_TERMINATE_PC        0x00402221
#define HANDOFF_LOCK            0xfd484380
#define HANDOFF_TID             128

        .macro  handoff_lock cpu, tmp, addr
        add     \cpu, \cpu, #HANDOFF_TID
        ldr     \addr, =HANDOFF_LOCK
1:      str     \cpu, [\addr]
        dsb
        ldr     \tmp, [\addr]
        cmp     \tmp, \cpu
        bne     1b
        sub     \cpu, \cpu, #HANDOFF_TID
        .endm

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
        mrc     p15, 0, r3, c0, c0, 5           /* MPIDR */
        and     r3, r3, #3
        handoff_lock r3, r0, r12
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
        bl      dcache_invalidate_all           /* L2 may have lost power */
        ldr     r1, =suspend_ctx
        ldr     r2, [r1]
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

/* Entry point of the secondary cores. They are only started so that the
 * secure world sees them in power collapse, the state it expects before
 * it lets the last core take the cluster down. Each one points its SAW
 * at the power collapse sequence, counts its entry and asks the secure
 * world to collapse it as a hotplugged core. Runs with the MMU off. */
#define SAW_CPU0                0xf9089000
#define SAW_SPM_CTL             0x30
#define SPM_CTL_POWER_COLLAPSE  0x59
#define SCM_CORE_HOTPLUGGED     0x10

        .text
        .global secondary_park
secondary_park:
        cpsid   if
        mrc     p15, 0, r4, c0, c0, 5           /* MPIDR */
        and     r4, r4, #3
        ldr     r5, =park_count
        add     r5, r5, r4, lsl #2
        ldr     r0, [r5]
        add     r0, r0, #1
        str     r0, [r5]
        ldr     r6, =SAW_CPU0
        add     r6, r6, r4, lsl #16
        mov     r0, #SPM_CTL_POWER_COLLAPSE
        str     r0, [r6, #SAW_SPM_CTL]
        dsb
        handoff_lock r4, r0, r7
        ldr     r0, =SCM_TERMINATE_PC
        mov     r1, r5
        mov     r2, #SCM_CORE_HOTPLUGGED
        smc     #0
        b       secondary_park

        .data
        .align  6
        .global park_count
park_count:
        .space  64
