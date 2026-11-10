/* What start.S does for this SoC only. */
        .macro  soc_entry
        .endm

        .macro  soc_cpu
        mrc     p15, 0, r0, c1, c0, 1
        orr     r0, r0, #(1 << 6)                          /* ACTLR.SMP */
        mcr     p15, 0, r0, c1, c0, 1
        isb
        .endm
