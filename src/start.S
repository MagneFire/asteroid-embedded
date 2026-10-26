/* Entry from the bootloader: non-secure SVC, MMU and caches off, IRQs
 * masked, r0 = 0, r1 = machine type, r2 = tags. The image starts with the
 * exception vectors so VBAR can point at the load address. */
        .syntax unified
        .arm

        .section .vectors, "ax"
        .global _start
_start:
        b       reset
        ldr     pc, =undef_entry
        ldr     pc, =svc_entry
        ldr     pc, =pabt_entry
        ldr     pc, =dabt_entry
        nop
        ldr     pc, =irq_entry
        ldr     pc, =fiq_entry

        .text
reset:
        cpsid   if
        ldr     sp, =__stack_svc_top
        bl      dcache_clean_invalidate_all
        mrc     p15, 0, r0, c1, c0, 0
        bic     r0, r0, #(1 << 0) | (1 << 2)               /* M, C */
        bic     r0, r0, #(1 << 11) | (1 << 12) | (1 << 13) /* Z, I, V */
        mcr     p15, 0, r0, c1, c0, 0
        isb
        mrc     p15, 0, r0, c1, c0, 1
        orr     r0, r0, #(1 << 6)                          /* ACTLR.SMP */
        mcr     p15, 0, r0, c1, c0, 1
        isb
        mov     r0, #0
        mcr     p15, 0, r0, c7, c5, 0           /* ICIALLU */
        mcr     p15, 0, r0, c7, c5, 6           /* BPIALL */
        mcr     p15, 0, r0, c8, c7, 0           /* TLBIALL */
        dsb
        isb

        ldr     r0, =_start
        mcr     p15, 0, r0, c12, c0, 0          /* VBAR */

        mrc     p15, 0, r0, c1, c0, 2
        orr     r0, r0, #(0xf << 20)            /* CPACR: cp10/cp11 full access */
        mcr     p15, 0, r0, c1, c0, 2
        isb
        mov     r0, #(1 << 30)
        vmsr    fpexc, r0                       /* FPEXC.EN */

        cps     #0x12
        ldr     sp, =__stack_irq_top
        cps     #0x17
        ldr     sp, =__stack_abt_top
        cps     #0x1b
        ldr     sp, =__stack_und_top
        cps     #0x11
        ldr     sp, =__stack_fiq_top
        cps     #0x13
        ldr     sp, =__stack_svc_top

        ldr     r0, =__bss_start
        ldr     r1, =__bss_end
        mov     r2, #0
1:      cmp     r0, r1
        strlo   r2, [r0], #4
        blo     1b

        bl      main
2:      wfi
        b       2b

/* Exceptions: save {r0-r12, lr (faulting pc + offset)} on the mode's stack
 * and hand it to C with the type; the handler never returns. */
        .macro  fault_entry name, type, adj
\name:
        sub     lr, lr, #\adj
        push    {r0-r12, lr}
        mov     r0, #\type
        mov     r1, sp
        bl      exception_handler
3:      b       3b
        .endm

        fault_entry undef_entry, 1, 4
        fault_entry svc_entry,   2, 0
        fault_entry pabt_entry,  3, 4
        fault_entry dabt_entry,  4, 8
        fault_entry fiq_entry,   5, 4

irq_entry:
        sub     lr, lr, #4
        push    {r0-r3, r12, lr}
        vmrs    r0, fpscr
        vpush   {d0-d7}
        push    {r0, r1}
        add     r0, sp, #72
        bl      irq_handler
        pop     {r0, r1}
        vpop    {d0-d7}
        vmsr    fpscr, r0
        ldmfd   sp!, {r0-r3, r12, pc}^

        .global irq_enable
irq_enable:
        cpsie   i
        bx      lr
        .global irq_disable
irq_disable:
        cpsid   i
        bx      lr
