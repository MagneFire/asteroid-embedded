/* host.c built for the Cortex-M4 (-DEMU), as emu.py loads it: everything
 * where it runs, with room for the frame and the largest heap. */
MEMORY {
	flash (rx) : ORIGIN = 0x00000000, LENGTH = 16M
	ram (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}
ENTRY(reset)
SECTIONS {
	.text : { *(.text .text.*) } > flash
	.rodata : { *(.rodata .rodata.*) } > flash
	.ARM.exidx : { *(.ARM.exidx .ARM.exidx.*) } > flash
	.data : { *(.data .data.*) } > ram
	.bss (NOLOAD) : { *(.bss .bss.* COMMON) } > ram
	/DISCARD/ : { *(.ARM.attributes) }
}
