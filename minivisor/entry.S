/*
 * entry.S - the two pieces of VMX that cannot be written in C.
 *
 * vmx_subvert(): the "blue pill". Writes GUEST_RSP/GUEST_RIP so that the
 *   guest resumes *right here*, on the caller's stack, then VMLAUNCH.
 *   Returns 0 when we come back as the guest, -1 if VMLAUNCH failed.
 *   Same idea as ksm's __vmx_vminit (vmx.S) and HFS Part 6
 *   AsmVmxSaveState/AsmVmxRestoreState.
 *
 * vmexit_stub: HOST_RIP. The CPU lands here on every VM exit with
 *   HOST_RSP = top of the VMM stack and *all guest GPRs still live*.
 *   Save them, call the C handler, restore, VMRESUME.
 *   Equivalent: ksm __vmx_entrypoint, HFS AsmVmexitHandler.
 */
#define GUEST_RSP	0x681c
#define GUEST_RIP	0x681e

	.text
	.code64

	.globl	vmx_subvert
vmx_subvert:
	push	%rbx
	push	%rbp
	push	%r12
	push	%r13
	push	%r14
	push	%r15
	pushfq
	mov	$GUEST_RSP, %rax
	vmwrite	%rsp, %rax
	lea	.Lguest_resume(%rip), %rdx
	mov	$GUEST_RIP, %rax
	vmwrite	%rdx, %rax
	vmlaunch
	/* Only reached if VMLAUNCH failed (CF or ZF set). */
	popfq
	pop	%r15
	pop	%r14
	pop	%r13
	pop	%r12
	pop	%rbp
	pop	%rbx
	mov	$-1, %eax
	ret
.Lguest_resume:
	/* We are now the guest. Same stack, same registers, same code. */
	popfq
	pop	%r15
	pop	%r14
	pop	%r13
	pop	%r12
	pop	%rbp
	pop	%rbx
	xor	%eax, %eax
	ret

	.globl	vmexit_stub
vmexit_stub:
	/* Build struct guest_regs (see main.c) on the VMM stack. */
	push	%r15
	push	%r14
	push	%r13
	push	%r12
	push	%r11
	push	%r10
	push	%r9
	push	%r8
	push	%rdi
	push	%rsi
	push	%rbp
	pushq	$0			/* rsp slot: real value lives in the VMCS */
	push	%rbx
	push	%rdx
	push	%rcx
	push	%rax
	mov	%rsp, %rdi
	call	vmexit_handler		/* bool: true = resume, false = devirt */
	test	%al, %al
	jz	.Ldevirt
	pop	%rax
	pop	%rcx
	pop	%rdx
	pop	%rbx
	add	$8, %rsp
	pop	%rbp
	pop	%rsi
	pop	%rdi
	pop	%r8
	pop	%r9
	pop	%r10
	pop	%r11
	pop	%r12
	pop	%r13
	pop	%r14
	pop	%r15
	vmresume
	call	vmresume_failed		/* noreturn */

.Ldevirt:
	/*
	 * The handler already executed VMXOFF and pushed the guest's next
	 * RIP onto the guest stack. Restore GPRs, switch to the guest stack,
	 * restore RFLAGS and "return" into the former guest, now native.
	 */
	pop	%rax
	pop	%rcx
	pop	%rdx
	pop	%rbx
	add	$8, %rsp
	pop	%rbp
	pop	%rsi
	pop	%rdi
	pop	%r8
	pop	%r9
	pop	%r10
	pop	%r11
	pop	%r12
	pop	%r13
	pop	%r14
	pop	%r15
	mov	devirt_rsp(%rip), %rsp
	pushq	devirt_rflags(%rip)
	popfq
	ret

/*
 * int vmx_run_separate(void): launch the *separate* guest (phase 2) from
 * the current VMCS. Returns 0 when the guest has finished (handler did
 * VMXOFF and resumed us through .Ldevirt with devirt_rsp = sep_host_rsp),
 * -1 if VMLAUNCH failed. Guest GPRs are whatever we hold now; the real-mode
 * guest initializes what it uses.
 */
	.globl	vmx_run_separate
vmx_run_separate:
	push	%rbx
	push	%rbp
	push	%r12
	push	%r13
	push	%r14
	push	%r15
	lea	.Lsep_done(%rip), %rax
	push	%rax			/* .Ldevirt will "ret" to this */
	mov	%rsp, sep_host_rsp(%rip)
	vmlaunch
	add	$8, %rsp		/* VMLAUNCH failed */
	pop	%r15
	pop	%r14
	pop	%r13
	pop	%r12
	pop	%rbp
	pop	%rbx
	mov	$-1, %eax
	ret
.Lsep_done:
	pop	%r15
	pop	%r14
	pop	%r13
	pop	%r12
	pop	%rbp
	pop	%rbx
	xor	%eax, %eax
	ret
