/* What start.S does for this SoC only. The bootloader loads the image
 * at an address of its own choosing and enters it there with the MMU
 * and the data cache off, so the first thing is to move to where the
 * image is linked. The device tree goes to the top of the image's
 * region before that: wherever the bootloader left it, the copy, the
 * bss or the stacks could land on it. Nothing here uses a stack. */
#include "memmap.h"

#define TREE_MAX        0x100000
#define TREE_COPY       (IMG_END - TREE_MAX)

        /* r0 = to, r1 = from, r2 = bytes; they may overlap. Uses r3. */
        .macro  move_words
        add     r2, r2, #3
        bic     r2, r2, #3
        cmp     r0, r1
        beq     3f
        bhi     2f
1:      subs    r2, r2, #4
        ldrge   r3, [r1], #4
        strge   r3, [r0], #4
        bgt     1b
        b       3f
2:      add     r1, r1, r2
        add     r0, r0, r2
4:      subs    r2, r2, #4
        ldrge   r3, [r1, #-4]!
        strge   r3, [r0, #-4]!
        bgt     4b
3:
        .endm

        .macro  soc_entry
        cmp     r8, #0
        beq     5f
        ldr     r0, [r8]
        ldr     r1, =0xedfe0dd0                 /* the tree's magic, as it reads here */
        cmp     r0, r1
        bne     5f
        ldr     r2, [r8, #4]
        rev     r2, r2
        cmp     r2, #TREE_MAX
        bhi     5f
        ldr     r0, =TREE_COPY
        mov     r1, r8
        mov     r8, r0
        move_words
5:      adr     r1, 6f
        ldr     r0, =6f
        sub     r1, r1, r0                      /* loaded this far from the link address */
        ldr     r0, =_start
        add     r1, r1, r0
        ldr     r2, =__image_end
        sub     r2, r2, r0
        move_words
        dsb
        mov     r0, #0
        mcr     p15, 0, r0, c7, c5, 0           /* ICIALLU */
        mcr     p15, 0, r0, c7, c5, 6           /* BPIALL */
        dsb
        isb
        ldr     pc, =6f
6:
        .endm

        /* ACTLR belongs to the secure side on this chip. */
        .macro  soc_cpu
        .endm

/* With tiny numbers flushed to zero and one kind of NaN, this core does
 * its single precision arithmetic in the NEON unit, several times as
 * fast as in its floating point unit proper. */
        .macro  soc_fpu
        mov     r0, #(3 << 24)
        vmsr    fpscr, r0
        .endm
