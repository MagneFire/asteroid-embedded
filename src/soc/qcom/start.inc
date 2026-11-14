/* What start.S does for this SoC only. The APQ8026's bootloader loads
 * the image where it is linked. The APQ8009W's puts it PHYS_OFFSET_8009
 * higher, where its DRAM starts, so the image turns the MMU on at once
 * with DRAM mapped down to the link address and the peripherals at
 * IO_8009_BASE, and goes on from there. The table it uses is outside
 * the bss, which start.S clears with the table live; mmu_init replaces
 * it. The device tree pointer moves with DRAM. */
#include "memmap.h"

        .macro  soc_cpu
        mrc     p15, 0, r0, c1, c0, 1
        orr     r0, r0, #(1 << 6)                          /* ACTLR.SMP */
        mcr     p15, 0, r0, c1, c0, 1
        isb
        .endm

        .macro  soc_entry
        adr     r0, 90f
        ldr     r1, =90f
        subs    r9, r0, r1
        beq     90f
        ldr     sp, =__stack_svc_top
        add     sp, sp, r9
        bl      dcache_clean_invalidate_all
        mrc     p15, 0, r0, c1, c0, 0
        bic     r0, r0, #(1 << 0) | (1 << 2)               /* M, C */
        bic     r0, r0, #(1 << 11) | (1 << 12) | (1 << 13) /* Z, I, V */
        mcr     p15, 0, r0, c1, c0, 0
        isb
        mov     r0, #0
        mcr     p15, 0, r0, c7, c5, 0           /* ICIALLU */
        mcr     p15, 0, r0, c8, c7, 0           /* TLBIALL */
        dsb
        isb
        soc_cpu

        ldr     r0, =__mmu_boot
        add     r0, r0, r9
        mov     r1, #0
        mov     r2, #4096
91:     subs    r2, r2, #1
        str     r1, [r0, r2, lsl #2]
        bne     91b
        /* DRAM at the link address, and where it is, for the next few
         * instructions after the MMU goes on. */
        ldr     r3, =0x1c0e                     /* MMU_RAM */
        orr     r3, r3, r9
        add     r4, r0, r9, lsr #18
        mov     r1, #0
92:     str     r3, [r0, r1, lsl #2]
        str     r3, [r4, r1, lsl #2]
        add     r3, r3, #0x100000
        add     r1, r1, #1
        cmp     r1, #(DRAM_END >> 20)
        blo     92b
        ldr     r3, =0x0c16                     /* MMU_DEVICE */
        mov     r1, #(IO_8009_BASE >> 20)
93:     str     r3, [r0, r1, lsl #2]
        add     r3, r3, #0x100000
        add     r1, r1, #1
        cmp     r1, #((IO_8009_BASE + IO_8009_SIZE) >> 20)
        blo     93b
        ldr     r3, =(RAMOOPS_8009_PHYS | 0x1c0e)  /* the log, as mmu_init maps it */
        str     r3, [r0, #((RAMOOPS_BASE >> 20) << 2)]

        mov     r1, #0
        mcr     p15, 0, r1, c2, c0, 2           /* TTBCR N=0 */
        mcr     p15, 0, r0, c2, c0, 0           /* TTBR0 */
        mov     r1, #1
        mcr     p15, 0, r1, c3, c0, 0           /* DACR: domain 0 client */
        mov     r1, #0
        mcr     p15, 0, r1, c8, c7, 0           /* TLBIALL */
        dsb
        isb
        mrc     p15, 0, r1, c1, c0, 0
        orr     r1, r1, #(1 << 0)               /* M */
        mcr     p15, 0, r1, c1, c0, 0
        isb
        ldr     pc, =94f
94:     ldr     r0, =phys_offset
        str     r9, [r0]
        cmp     r8, #0
        subne   r8, r8, r9
        mov     r7, #1
90:
        .endm

        .macro  soc_fpu
        .endm
