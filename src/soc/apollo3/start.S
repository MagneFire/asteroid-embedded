/* Entry from the boot loader, which jumps through the vector table at
 * the start of the image with the core in thread mode on the main stack.
 * Every interrupt goes to irq_entry, which hands the line's number and
 * the stacked frame to irq_handler; faults go to fault_entry. The FPU's
 * registers are stacked lazily by the core itself. */
        .syntax unified
        .thumb
        .cpu    cortex-m4
        .fpu    fpv4-sp-d16

        .section .vectors, "a"
        .global _start
_start:
        .word   __stack_top
        .word   reset
        .word   fault_entry             /* NMI */
        .word   fault_entry             /* HardFault */
        .word   fault_entry             /* MemManage */
        .word   fault_entry             /* BusFault */
        .word   fault_entry             /* UsageFault */
        .word   0, 0, 0, 0
        .word   fault_entry             /* SVCall */
        .word   fault_entry             /* DebugMon */
        .word   0
        .word   fault_entry             /* PendSV */
        .word   irq_entry               /* SysTick */
        .rept   32
        .word   irq_entry
        .endr

        .text
        .thumb_func
reset:
        cpsid   i
        ldr     r0, =_start
        ldr     r1, =0xe000ed08         /* VTOR */
        str     r0, [r1]
        ldr     r0, =__stack_top
        msr     msp, r0
        ldr     r1, =0xe000ed88         /* CPACR: cp10/cp11 full access */
        ldr     r0, [r1]
        orr     r0, r0, #(0xf << 20)
        str     r0, [r1]
        dsb
        isb

        ldr     r0, =__data_start
        ldr     r1, =__data_load
        ldr     r2, =__data_end
1:      cmp     r0, r2
        ittt    lo
        ldrlo   r3, [r1], #4
        strlo   r3, [r0], #4
        blo     1b

        ldr     r0, =__bss_start
        ldr     r1, =__bss_end
        movs    r2, #0
2:      cmp     r0, r1
        itt     lo
        strlo   r2, [r0], #4
        blo     2b

        movs    r0, #0
        movs    r1, #0
        bl      main
3:      wfi
        b       3b

/* The frame is on the main stack: the core pushed {r0-r3, r12, lr, pc,
 * xpsr} there, and with the FPU's state behind it if the frame's bit 4 of
 * EXC_RETURN is clear. */
        .thumb_func
        .global irq_entry
irq_entry:
        push    {r4, lr}
        mrs     r0, ipsr
        add     r1, sp, #8
        bl      irq_handler
        pop     {r4, pc}

/* r4-r11 go below the core's frame, so the record has every register. */
        .thumb_func
fault_entry:
        cpsid   i
        tst     lr, #4
        ite     eq
        mrseq   r1, msp
        mrsne   r1, psp
        push    {r4-r11}
        mov     r2, sp
        mrs     r0, ipsr
        bl      fault_handler
4:      b       4b

        .thumb_func
        .global irq_enable
irq_enable:
        cpsie   i
        bx      lr
        .thumb_func
        .global irq_disable
irq_disable:
        cpsid   i
        bx      lr
        .thumb_func
        .global irq_save
irq_save:
        mrs     r0, primask
        cpsid   i
        bx      lr
        .thumb_func
        .global irq_restore
irq_restore:
        msr     primask, r0
        bx      lr
