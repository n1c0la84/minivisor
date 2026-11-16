/* Image loads at 0x7c00 (boot sector) + 0x7e00.. (rest). BSS lives at 1 MiB. */
ENTRY(_start)
SECTIONS
{
	. = 0x7c00;
	.boot : { *(.boot) . = 0x7c00 + 0x1fe; SHORT(0xaa55) }
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	_image_end = .;
	_load_sectors = (_image_end - 0x7e00 + 511) / 512;
	. = 0x100000;
	.bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) __bss_end = .; }
	/DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
