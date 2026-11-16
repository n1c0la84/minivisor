/*
 * guest16.S - a tiny real-mode program run as a *separate* VM (phase 2).
 *
 * The host copies these bytes to guest-physical 0x1000 of a 64 KiB guest
 * and starts it at CS:IP = 0000:1000. It knows nothing about the host:
 * it only does port I/O, one VMCALL, and memory accesses. Addresses are
 * written as (label - guest16_start + 0x1000), i.e. guest-physical.
 */
#define G(x) ((x) - guest16_start + 0x1000)

	.section .rodata.guest16, "a"
	.code16
	.globl	guest16_start, guest16_end
guest16_start:
	mov	$G(msg_hello), %si
	call	print

	/* sum 1..100, report it to the host with a hypercall */
	xor	%bx, %bx
	mov	$100, %cx
1:	add	%cx, %bx
	loop	1b
	mov	$2, %ax			/* HCALL_REPORT, value in bx */
	vmcall

	/* Touch guest-physical 0xb8000: not backed by any RAM in our EPT. */
	mov	$G(msg_touch), %si
	call	print
	mov	$0xb800, %ax
	mov	%ax, %ds
	movw	$0x4b4f, 0		/* "OK" -> EPT violation, host maps a page */
	mov	0, %dx			/* read it back from the new page */
	xor	%ax, %ax
	mov	%ax, %ds
	mov	%dl, G(msg_back) + 10
	mov	%dh, G(msg_back) + 11
	mov	$G(msg_back), %si
	call	print

	mov	$G(msg_bye), %si
	call	print
	hlt				/* host ends the VM here */

print:	lodsb
	test	%al, %al
	jz	2f
	out	%al, $0xe9
	jmp	print
2:	ret

msg_hello:	.asciz "real-mode guest up, CS:IP=0000:1000, 64 KiB of RAM\n"
msg_touch:	.asciz "writing to 0xb8000, which has no RAM behind it...\n"
msg_back:	.asciz "read back ??, the page exists now\n"
msg_bye:	.asciz "guest done, executing HLT\n"
guest16_end:
