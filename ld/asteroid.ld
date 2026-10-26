/* The bootloader copies the image to 0x8000 and jumps to its first word.
 * Everything the firmware owns sits in the first DRAM bank (0 - 0x02f00000);
 * the inherited framebuffer and the persistent log are fixed addresses
 * elsewhere (src/soc/msm8226.h). Code, data, bss and stacks must end below
 * 0x01e00000, where the bootloader puts the device tree. */
MEMORY
{
    IMG (rwx) : ORIGIN = 0x00008000, LENGTH = 0x01e00000 - 0x00008000
}
ENTRY(_start)
SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > IMG
    .data : { . = ALIGN(4); *(.data*) . = ALIGN(4); } > IMG
    __image_end = .;
    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(16384);
        __mmu_l1 = .;
        . += 16384;
        . = ALIGN(4);
        __bss_end = .;
    } > IMG
    .stacks (NOLOAD) : {
        . = ALIGN(16);
        . += 256;   __stack_fiq_top = .;
        . += 1024;  __stack_und_top = .;
        . += 4096;  __stack_abt_top = .;
        . += 4096;  __stack_irq_top = .;
        . += 32768; __stack_svc_top = .;
    } > IMG
    __ram_end = .;
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
