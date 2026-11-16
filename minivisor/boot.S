/*
 * boot.S - from BIOS boot sector to 64-bit C, the shortest sane path.
 *
 *   real mode (0x7c00) -> load rest of image -> A20 -> protected mode
 *   -> identity-map 4 GiB with 2 MiB pages -> long mode -> kmain()
 *
 * Nothing here is VMX-specific; it just gives us what a kernel driver
 * would already have on Windows/Linux: 64-bit mode, paging, a GDT.
 */

	.section .boot, "ax"
	.code16
	.globl	_start
_start:
	cli
	xor	%ax, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	mov	$0x7c00, %sp
	ljmp	$0, $1f
1:	mov	%dl, boot_drive

	/* Read the rest of the image (LBA 1..n) to 0x7e00, one sector at a time. */
	mov	$_load_sectors, %cx
.Lread:
	jcxz	.Lloaded
	push	%cx
	mov	$0x42, %ah
	mov	boot_drive, %dl
	mov	$dap, %si
	int	$0x13
	pop	%cx
	jc	.Lfail
	incw	dap_lba
	addw	$0x20, dap_seg
	dec	%cx
	jmp	.Lread

.Lloaded:
	in	$0x92, %al		/* fast A20 */
	or	$2, %al
	and	$0xfe, %al
	out	%al, $0x92

	lgdt	gdt_desc
	mov	%cr0, %eax
	or	$1, %eax
	mov	%eax, %cr0
	ljmp	$0x18, $pm32		/* 32-bit code segment */

.Lfail:
	mov	$'!', %al
	out	%al, $0xe9
	hlt
	jmp	.Lfail

boot_drive:	.byte 0
	.balign 4
dap:		.byte 0x10, 0
		.word 1			/* sectors per call */
		.word 0			/* offset */
dap_seg:	.word 0x07e0		/* segment */
dap_lba:	.quad 1

/* ------------------------------------------------------------------ */
	.text
	.code32
pm32:
	mov	$0x10, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	mov	%ax, %fs
	mov	%ax, %gs

	/* Page tables at 0x1000..0x6fff: PML4, PDPT, 4 x PD. */
	mov	$0x1000, %edi
	xor	%eax, %eax
	mov	$(0x6000 / 4), %ecx
	rep stosl
	movl	$0x2003, 0x1000			/* PML4[0] -> PDPT, P|RW */
	movl	$0x3003, 0x2000			/* PDPT[0..3] -> PD0..PD3 */
	movl	$0x4003, 0x2008
	movl	$0x5003, 0x2010
	movl	$0x6003, 0x2018
	mov	$0x3000, %edi			/* 2048 x 2 MiB = 4 GiB identity */
	xor	%ebx, %ebx
.Lpd:	mov	%ebx, %eax
	shl	$21, %eax
	or	$0x83, %eax			/* P|RW|PS */
	mov	%eax, (%edi)
	add	$8, %edi
	inc	%ebx
	cmp	$2048, %ebx
	jne	.Lpd

	mov	%cr4, %eax
	or	$0x20, %eax			/* PAE */
	mov	%eax, %cr4
	mov	$0x1000, %eax
	mov	%eax, %cr3
	mov	$0xc0000080, %ecx		/* EFER.LME */
	rdmsr
	or	$0x100, %eax
	wrmsr
	mov	%cr0, %eax
	or	$0x80000000, %eax		/* PG */
	mov	%eax, %cr0
	ljmp	$0x08, $lm64

	.code64
lm64:
	mov	$0x10, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	mov	%ax, %fs
	mov	%ax, %gs
	mov	$__bss_start, %rdi
	mov	$__bss_end, %rcx
	sub	%rdi, %rcx
	xor	%eax, %eax
	rep stosb
	mov	$host_stack_top, %rsp
	call	kmain
1:	cli
	hlt
	jmp	1b

/* ------------------------------------------------------------------ */
	.data
	.balign 16
	.globl	gdt
gdt:
	.quad	0				/* 0x00 */
	.quad	0x00af9a000000ffff		/* 0x08 code64 */
	.quad	0x00cf92000000ffff		/* 0x10 data   */
	.quad	0x00cf9a000000ffff		/* 0x18 code32 */
	.quad	0, 0				/* 0x20 TSS (filled by kmain) */
gdt_end:
	.globl	gdt_desc
gdt_desc:
	.word	gdt_end - gdt - 1
	.quad	gdt

	.bss
	.balign 16
	.space	16384
	.globl	host_stack_top
host_stack_top:
