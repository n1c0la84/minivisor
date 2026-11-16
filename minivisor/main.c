/*
 * minivisor - a ~500-line thin hypervisor for Intel VT-x, bare metal.
 *
 * It does what Hypervisor-From-Scratch (Parts 2-8) and ksm do inside a
 * Windows/Linux kernel, minus the OS:
 *
 *   1. check VMX, IA32_FEATURE_CONTROL, CR0/CR4 fixed bits   (HFS 2, ksm.c)
 *   2. VMXON with a revision-tagged region                   (HFS 2)
 *   3. VMCLEAR + VMPTRLD a VMCS, fill host/guest/control     (HFS 3,5; vcpu.c)
 *   4. build an identity EPT, one page write-protected       (HFS 4,7; vcpu.c)
 *   5. VMLAUNCH with GUEST_RIP = "right after the launch"    (HFS 6; vmx.S)
 *      -> the code that called us keeps running, now as a guest
 *   6. handle exits: CPUID, I/O, HLT, VMCALL, EPT, MTF       (HFS 6-8; exit.c)
 *   7. a VMCALL turns the hypervisor off and returns the CPU (HFS 6; ksm.c)
 *
 * Output goes to Bochs' port 0xE9. The guest's own 0xE9 writes are trapped
 * through the I/O bitmap, so every guest line shows up prefixed "[guest]".
 */
#include <stdarg.h>
#include "vmx.h"

#define PAGE_SIZE 4096

/* The compiler may emit calls to these even in freestanding code. */
void *memcpy(void *d, const void *src, unsigned long n)
{
	u8 *o = d; const u8 *i = src;
	while (n--) *o++ = *i++;
	return d;
}
#define ALIGNED(n) __attribute__((aligned(n)))

/* ------------------------------------------------------------------ */
/* Console                                                             */

static void puts_raw(const char *s) { while (*s) outb(0xe9, *s++); }

static void put_num(u64 v, int base, int width)
{
	char buf[24];
	int i = 0;
	do {
		buf[i++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	while (i < width) buf[i++] = '0';
	while (i) outb(0xe9, buf[--i]);
}

/* %s %c %d %u %x, optional width digit and 'l' (ignored: all 64-bit). */
static void kprintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') { outb(0xe9, *fmt); continue; }
		int width = 0;
		fmt++;
		while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l') fmt++;
		switch (*fmt) {
		case 's': puts_raw(va_arg(ap, const char *)); break;
		case 'c': outb(0xe9, (char)va_arg(ap, int)); break;
		case 'd': { long v = va_arg(ap, long); if (v < 0) { outb(0xe9, '-'); v = -v; } put_num(v, 10, width); break; }
		case 'u': put_num(va_arg(ap, u64), 10, width); break;
		case 'x': put_num(va_arg(ap, u64), 16, width); break;
		default: outb(0xe9, *fmt);
		}
	}
	va_end(ap);
}

static void shutdown(void)
{
	for (const char *s = "Shutdown"; *s; s++) outb(0x8900, *s);	/* Bochs */
	for (;;) __asm__ volatile("cli; hlt");
}

__attribute__((noreturn)) static void panic(const char *what)
{
	kprintf("\n[host] PANIC: %s\n", what);
	shutdown();
	__builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/* Memory the hypervisor owns. Identity-mapped, so VA == PA.           */

static u8  vmxon_region[PAGE_SIZE] ALIGNED(PAGE_SIZE);
static u8  vmcs_region[PAGE_SIZE] ALIGNED(PAGE_SIZE);
static u8  io_bitmap_a[PAGE_SIZE] ALIGNED(PAGE_SIZE);	/* ports 0000-7fff */
static u8  io_bitmap_b[PAGE_SIZE] ALIGNED(PAGE_SIZE);	/* ports 8000-ffff */
static u8  msr_bitmap[PAGE_SIZE] ALIGNED(PAGE_SIZE);	/* all zero: no MSR exits */
static u64 ept_pml4[512] ALIGNED(PAGE_SIZE);
static u64 ept_pdpt[512] ALIGNED(PAGE_SIZE);
static u64 ept_pd[4][512] ALIGNED(PAGE_SIZE);
static u64 ept_pt[512] ALIGNED(PAGE_SIZE);		/* one split 2 MiB region */
static u8  vmm_stack[16384] ALIGNED(16);

/* The page the guest will write to and the hypervisor will watch. */
static volatile u8 watched_page[PAGE_SIZE] ALIGNED(PAGE_SIZE);

struct tss64 {
	u32 rsvd0; u64 rsp[3]; u64 rsvd1; u64 ist[7]; u64 rsvd2; u16 rsvd3, iomap;
} __attribute__((packed));
static struct tss64 tss ALIGNED(16);

extern u64 gdt[];
extern struct { u16 limit; u64 base; } __attribute__((packed)) gdt_desc;

/* ------------------------------------------------------------------ */
/* State shared by the exit handler                                    */

struct guest_regs {	/* layout fixed by vmexit_stub in entry.S */
	u64 rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
	u64 r8, r9, r10, r11, r12, r13, r14, r15;
};

static u64 exit_count[EXIT_REASON_MAX];
static u64 *watched_pte;
static u64 last_violation_gpa;
static bool guest_line_start = true;
static const char *guest_tag = "[guest] ";
static int phase = 1;			/* 1 = blue pill, 2 = separate VM */
u64 sep_host_rsp;			/* written by vmx_run_separate */
u64 devirt_rsp, devirt_rflags;		/* read by entry.S */

int  vmx_subvert(void);			/* entry.S */
int  vmx_run_separate(void);		/* entry.S */
void vmexit_stub(void);			/* entry.S */

/* ------------------------------------------------------------------ */
/* Step 1+2: enable VMX operation                                      */

static void enable_vmx(void)
{
	u32 a, b, c, d;
	cpuid(1, &a, &b, &c, &d);
	if (!(c & (1u << 5)))
		panic("CPUID.1:ECX.VMX = 0, no VT-x (is Bochs built with --enable-vmx?)");

	/* Firmware normally locks this MSR. If unlocked, we enable+lock it. */
	u64 fc = rdmsr(MSR_IA32_FEATURE_CONTROL);
	u64 need = FEATURE_CONTROL_LOCKED | FEATURE_CONTROL_VMXON_OUTSIDE_SMX;
	if ((fc & need) != need) {
		if (fc & FEATURE_CONTROL_LOCKED)
			panic("VMX disabled and locked in IA32_FEATURE_CONTROL");
		wrmsr(MSR_IA32_FEATURE_CONTROL, fc | need);
	}

	/* CR0/CR4 bits that must be 1 (FIXED0) or may be 1 (FIXED1) in VMX. */
	write_cr0((read_cr0() | rdmsr(MSR_IA32_VMX_CR0_FIXED0)) & rdmsr(MSR_IA32_VMX_CR0_FIXED1));
	write_cr4((read_cr4() | CR4_VMXE | rdmsr(MSR_IA32_VMX_CR4_FIXED0)) & rdmsr(MSR_IA32_VMX_CR4_FIXED1));

	u64 basic = rdmsr(MSR_IA32_VMX_BASIC);
	kprintf("[host] IA32_VMX_BASIC = %x (VMCS revision %x)\n", basic, basic & 0x7fffffff);

	*(u32 *)vmxon_region = basic & 0x7fffffff;
	if (!vmxon((u64)vmxon_region))
		panic("VMXON failed");
	kprintf("[host] VMXON ok, region at %x\n", (u64)vmxon_region);
}

/* ------------------------------------------------------------------ */
/* Step 4: EPT. Guest-physical == host-physical for 0-4 GiB.           */

#define EPT_R (1ull << 0)
#define EPT_W (1ull << 1)
#define EPT_X (1ull << 2)
#define EPT_RWX (EPT_R | EPT_W | EPT_X)
#define EPT_MT_WB (6ull << 3)		/* leaf memory type */
#define EPT_LARGE (1ull << 7)

static u64 build_ept(void)
{
	u64 cap = rdmsr(MSR_IA32_VMX_EPT_VPID_CAP);
	kprintf("[host] IA32_VMX_EPT_VPID_CAP = %x\n", cap);
	if (!(cap & (1ull << 6)) || !(cap & (1ull << 14)) || !(cap & (1ull << 16)))
		panic("need EPT 4-level walk, WB memory type and 2 MiB pages");

	ept_pml4[0] = (u64)ept_pdpt | EPT_RWX;
	for (int i = 0; i < 4; i++) {
		ept_pdpt[i] = (u64)ept_pd[i] | EPT_RWX;
		for (int j = 0; j < 512; j++) {
			u64 gpa = ((u64)i * 512 + j) << 21;
			u64 mt = gpa < 0xc0000000ull ? EPT_MT_WB : 0;	/* MMIO hole: UC */
			ept_pd[i][j] = gpa | EPT_RWX | mt | EPT_LARGE;
		}
	}

	/* Split the 2 MiB page holding watched_page into 512 x 4 KiB ... */
	u64 wpa = (u64)watched_page, base = wpa & ~((1ull << 21) - 1);
	for (int k = 0; k < 512; k++)
		ept_pt[k] = (base + (u64)k * PAGE_SIZE) | EPT_RWX | EPT_MT_WB;
	ept_pd[base >> 30][(base >> 21) & 511] = (u64)ept_pt | EPT_RWX;

	/* ... and make that one 4 KiB page read+execute only. */
	watched_pte = &ept_pt[(wpa >> 12) & 511];
	*watched_pte &= ~EPT_W;
	kprintf("[host] EPT: 4 GiB identity map, page %x is write-protected\n", wpa);

	/* EPTP: PML4 address | (walk length - 1) << 3 | WB */
	return (u64)ept_pml4 | (3 << 3) | 6;
}

/* ------------------------------------------------------------------ */
/* Step 3: the VMCS                                                    */

/* Apply a capability MSR: low dword = must-be-1, high dword = may-be-1. */
static u32 adjust(u32 want, u32 msr)
{
	u64 cap = rdmsr(msr);
	return (want | (u32)cap) & (u32)(cap >> 32);
}

/* Tag a region with the revision id, VMCLEAR it, make it the current VMCS. */
static void setup_vmcs_region(u8 *region)
{
	*(u32 *)region = rdmsr(MSR_IA32_VMX_BASIC) & 0x7fffffff;
	if (!vmclear((u64)region) || !vmptrld((u64)region))
		panic("VMCLEAR/VMPTRLD failed");
}

/* Controls + host state: identical for both of our VMs. */
static void setup_vmcs_common(u64 eptp)
{
	bool true_ctls = rdmsr(MSR_IA32_VMX_BASIC) & (1ull << 55);
	u32 m_pin   = true_ctls ? MSR_IA32_VMX_TRUE_PINBASED  : MSR_IA32_VMX_PINBASED_CTLS;
	u32 m_proc  = true_ctls ? MSR_IA32_VMX_TRUE_PROCBASED : MSR_IA32_VMX_PROCBASED_CTLS;
	u32 m_exit  = true_ctls ? MSR_IA32_VMX_TRUE_EXIT      : MSR_IA32_VMX_EXIT_CTLS;
	u32 m_entry = true_ctls ? MSR_IA32_VMX_TRUE_ENTRY     : MSR_IA32_VMX_ENTRY_CTLS;

	/* -- controls: what makes the guest exit -- */
	vmwrite(PIN_BASED_VM_EXEC_CONTROL, adjust(0, m_pin));
	vmwrite(CPU_BASED_VM_EXEC_CONTROL, adjust(CPU_HLT_EXITING | CPU_USE_IO_BITMAPS |
						  CPU_USE_MSR_BITMAPS | CPU_SECONDARY, m_proc));
	vmwrite(SECONDARY_VM_EXEC_CONTROL, adjust(CPU2_ENABLE_EPT, MSR_IA32_VMX_PROCBASED_CTLS2));
	vmwrite(VM_EXIT_CONTROLS, adjust(EXIT_HOST_ADDR_SPACE_SIZE, m_exit));
	vmwrite(VM_ENTRY_CONTROLS, adjust(ENTRY_IA32E_MODE_GUEST, m_entry));
	vmwrite(EXCEPTION_BITMAP, 0xffffffff);	/* any guest exception -> exit (debug aid) */
	vmwrite(CR3_TARGET_COUNT, 0);
	vmwrite(CR0_GUEST_HOST_MASK, 0);	/* guest owns all CR0/CR4 bits */
	vmwrite(CR4_GUEST_HOST_MASK, 0);
	vmwrite(VM_EXIT_MSR_STORE_COUNT, 0);
	vmwrite(VM_EXIT_MSR_LOAD_COUNT, 0);
	vmwrite(VM_ENTRY_MSR_LOAD_COUNT, 0);
	vmwrite(VM_ENTRY_INTR_INFO, 0);

	io_bitmap_a[0xe9 / 8] |= 1 << (0xe9 % 8);	/* trap guest debug-port writes */
	vmwrite(IO_BITMAP_A, (u64)io_bitmap_a);
	vmwrite(IO_BITMAP_B, (u64)io_bitmap_b);
	vmwrite(MSR_BITMAP, (u64)msr_bitmap);
	vmwrite(EPT_POINTER, eptp);
	vmwrite(VMCS_LINK_POINTER, ~0ull);

	/* -- host state: loaded on every VM exit -- */
	vmwrite(HOST_CR0, read_cr0());
	vmwrite(HOST_CR3, read_cr3());
	vmwrite(HOST_CR4, read_cr4());
	vmwrite(HOST_CS_SELECTOR, 0x08);
	vmwrite(HOST_SS_SELECTOR, 0x10);
	vmwrite(HOST_DS_SELECTOR, 0x10);
	vmwrite(HOST_ES_SELECTOR, 0x10);
	vmwrite(HOST_FS_SELECTOR, 0x10);
	vmwrite(HOST_GS_SELECTOR, 0x10);
	vmwrite(HOST_TR_SELECTOR, 0x20);
	vmwrite(HOST_FS_BASE, 0);
	vmwrite(HOST_GS_BASE, 0);
	vmwrite(HOST_TR_BASE, (u64)&tss);
	vmwrite(HOST_GDTR_BASE, (u64)gdt);
	vmwrite(HOST_IDTR_BASE, 0);
	vmwrite(HOST_SYSENTER_CS, 0);
	vmwrite(HOST_SYSENTER_ESP, 0);
	vmwrite(HOST_SYSENTER_EIP, 0);
	vmwrite(HOST_RSP, (u64)vmm_stack + sizeof(vmm_stack));
	vmwrite(HOST_RIP, (u64)vmexit_stub);
}

static void setup_vmcs(u64 eptp)
{
	setup_vmcs_region(vmcs_region);
	setup_vmcs_common(eptp);

	/* -- guest state: a copy of *our own* current state -- */
	vmwrite(GUEST_CR0, read_cr0());
	vmwrite(GUEST_CR3, read_cr3());
	vmwrite(GUEST_CR4, read_cr4());
	vmwrite(CR0_READ_SHADOW, read_cr0());
	vmwrite(CR4_READ_SHADOW, read_cr4());
	vmwrite(GUEST_DR7, 0x400);
	vmwrite(GUEST_RFLAGS, read_rflags());
	/* GUEST_RSP / GUEST_RIP are written by vmx_subvert() just before launch. */

	/* Access rights: type | S<<4 | DPL<<5 | P<<7 | L<<13 | D/B<<14 | G<<15 */
	static const struct { u32 sel, lim, ar, base; } seg[] = {
		[0] = { GUEST_ES_SELECTOR, GUEST_ES_LIMIT, GUEST_ES_AR, GUEST_ES_BASE },
		[1] = { GUEST_CS_SELECTOR, GUEST_CS_LIMIT, GUEST_CS_AR, GUEST_CS_BASE },
		[2] = { GUEST_SS_SELECTOR, GUEST_SS_LIMIT, GUEST_SS_AR, GUEST_SS_BASE },
		[3] = { GUEST_DS_SELECTOR, GUEST_DS_LIMIT, GUEST_DS_AR, GUEST_DS_BASE },
		[4] = { GUEST_FS_SELECTOR, GUEST_FS_LIMIT, GUEST_FS_AR, GUEST_FS_BASE },
		[5] = { GUEST_GS_SELECTOR, GUEST_GS_LIMIT, GUEST_GS_AR, GUEST_GS_BASE },
	};
	for (int i = 0; i < 6; i++) {
		bool code = (i == 1);
		vmwrite(seg[i].sel, code ? 0x08 : 0x10);
		vmwrite(seg[i].lim, 0xffffffff);
		vmwrite(seg[i].ar, code ? 0xa09b : 0xc093);
		vmwrite(seg[i].base, 0);
	}
	vmwrite(GUEST_LDTR_SELECTOR, 0);
	vmwrite(GUEST_LDTR_AR, 0x10000);		/* unusable */
	vmwrite(GUEST_LDTR_LIMIT, 0);
	vmwrite(GUEST_LDTR_BASE, 0);
	vmwrite(GUEST_TR_SELECTOR, 0x20);
	vmwrite(GUEST_TR_LIMIT, sizeof(tss) - 1);
	vmwrite(GUEST_TR_AR, 0x8b);			/* busy 64-bit TSS, present */
	vmwrite(GUEST_TR_BASE, (u64)&tss);
	vmwrite(GUEST_GDTR_BASE, gdt_desc.base);
	vmwrite(GUEST_GDTR_LIMIT, gdt_desc.limit);
	vmwrite(GUEST_IDTR_BASE, 0);
	vmwrite(GUEST_IDTR_LIMIT, 0);
	vmwrite(GUEST_IA32_DEBUGCTL, 0);
	vmwrite(GUEST_SYSENTER_CS, 0);
	vmwrite(GUEST_SYSENTER_ESP, 0);
	vmwrite(GUEST_SYSENTER_EIP, 0);
	vmwrite(GUEST_INTERRUPTIBILITY, 0);
	vmwrite(GUEST_ACTIVITY_STATE, 0);
	vmwrite(GUEST_PENDING_DBG, 0);
}

/* ------------------------------------------------------------------ */
/* Step 6: the VM-exit handler (called from vmexit_stub)               */

static void advance_rip(void)
{
	vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTRUCTION_LEN));
}

static void handle_cpuid(struct guest_regs *r)
{
	u32 leaf = r->rax, a, b, c, d;
	if (leaf == 0x40000000) {
		/* Hypervisor vendor leaf: what real hypervisors expose. */
		a = 0x40000000;
		__builtin_memcpy(&b, "Mini", 4);
		__builtin_memcpy(&c, "Viso", 4);
		__builtin_memcpy(&d, "rHV!", 4);
	} else {
		cpuid(leaf, &a, &b, &c, &d);	/* executes natively in root mode */
		if (leaf == 1) {
			c |= 1u << 31;		/* "hypervisor present" */
			c &= ~(1u << 5);	/* hide VMX from the guest */
		}
	}
	r->rax = a; r->rbx = b; r->rcx = c; r->rdx = d;
	advance_rip();
}

static void handle_io(struct guest_regs *r)
{
	u64 q = vmread(EXIT_QUALIFICATION);
	u16 port = q >> 16;
	bool in = q & 8, string = q & 16;
	if (port == 0xe9 && !in && !string) {
		char ch = r->rax & 0xff;
		if (guest_line_start) puts_raw(guest_tag);
		outb(0xe9, ch);			/* forward it, with a prefix */
		guest_line_start = (ch == '\n');
	} else {
		kprintf("[host] unexpected I/O exit, port %x\n", (u64)port);
	}
	advance_rip();
}

static void set_watch(bool writable)
{
	if (writable) *watched_pte |= EPT_W;
	else          *watched_pte &= ~EPT_W;
	invept_all();	/* drop cached EPT translations */
}

static void handle_ept_violation(void)
{
	u64 q = vmread(EXIT_QUALIFICATION);
	u64 gpa = vmread(GUEST_PHYSICAL_ADDRESS);
	last_violation_gpa = gpa;
	kprintf("[host] EPT violation: %s at GPA %x, RIP %x\n",
		(q & 2) ? "write" : (q & 4) ? "fetch" : "read", gpa, vmread(GUEST_RIP));

	/*
	 * Let this one instruction through: make the page writable, set the
	 * monitor trap flag, re-enter. After one guest instruction we get an
	 * MTF exit and protect the page again. HFS Part 8 and ksm's epage.c
	 * build their hooks on the same trick.
	 */
	set_watch(true);
	vmwrite(CPU_BASED_VM_EXEC_CONTROL, vmread(CPU_BASED_VM_EXEC_CONTROL) | CPU_MTF);
	/* RIP is NOT advanced: the faulting write is re-executed. */
}

static void handle_mtf(void)
{
	vmwrite(CPU_BASED_VM_EXEC_CONTROL, vmread(CPU_BASED_VM_EXEC_CONTROL) & ~CPU_MTF);
	set_watch(false);
	kprintf("[host] MTF: write retired, page re-protected ([%x] = %x)\n",
		last_violation_gpa, (u64)*(volatile u8 *)last_violation_gpa);
}

#define HCALL_PING	0x1
#define HCALL_DEVIRT	0xdead

/* Returns false to devirtualize (stub then resumes the guest natively). */
static bool handle_vmcall(struct guest_regs *r)
{
	switch (r->rax) {
	case HCALL_PING:
		kprintf("[host] VMCALL ping (rbx=%x)\n", r->rbx);
		r->rax = 0x1337;
		advance_rip();
		return true;
	case HCALL_DEVIRT: {
		kprintf("[host] VMCALL devirtualize: VMXOFF, handing the CPU back\n");
		u64 rip = vmread(GUEST_RIP) + vmread(VM_EXIT_INSTRUCTION_LEN);
		u64 rsp = vmread(GUEST_RSP) - 8;
		*(u64 *)rsp = rip;		/* "ret" in the stub lands here */
		devirt_rsp = rsp;
		devirt_rflags = vmread(GUEST_RFLAGS);
		/*
		 * A VM exit set GDTR/IDTR limits to 0xffff; a real hypervisor
		 * (ksm __ksm_exit_cpu, HFS RestoreRegisters) reloads the guest's
		 * GDTR/IDTR/CR3 here. Ours are identical to the host's, so skip.
		 */
		vmxoff();
		write_cr4(read_cr4() & ~CR4_VMXE);
		r->rax = 0;
		return false;
	}
	default:
		r->rax = (u64)-1;
		advance_rip();
		return true;
	}
}

/* ------------------------------------------------------------------ */
/* Phase 2: a separate VM. Own guest-physical memory, own EPT.         */

#define VM2_RAM_PAGES 16				/* 64 KiB */
static u8  vm2_ram[VM2_RAM_PAGES][PAGE_SIZE] ALIGNED(PAGE_SIZE);
static u8  vm2_spare[4][PAGE_SIZE] ALIGNED(PAGE_SIZE);	/* for demand mapping */
static int vm2_spare_used;
static u64 ept2_pml4[512] ALIGNED(PAGE_SIZE);
static u64 ept2_pdpt[512] ALIGNED(PAGE_SIZE);
static u64 ept2_pd[512] ALIGNED(PAGE_SIZE);
static u64 ept2_pt[512] ALIGNED(PAGE_SIZE);		/* GPA 0 - 2 MiB */
static u8  vmcs2_region[PAGE_SIZE] ALIGNED(PAGE_SIZE);

extern const u8 guest16_start[], guest16_end[];

#define HCALL_REPORT 0x2

/* Handles exits that mean something different for VM2. */
static bool vm2_exit(u32 reason, struct guest_regs *r, bool *resume)
{
	switch (reason) {
	case EXIT_EPT_VIOLATION: {
		u64 gpa = vmread(GUEST_PHYSICAL_ADDRESS);
		u64 q = vmread(EXIT_QUALIFICATION);
		if (gpa >= (2ull << 20) || vm2_spare_used == 4) {
			kprintf("[host] VM2 accessed GPA %x outside anything we can back\n", gpa);
			panic("vm2 EPT violation");
		}
		u8 *page = vm2_spare[vm2_spare_used++];
		ept2_pt[gpa >> 12] = (u64)page | EPT_RWX | EPT_MT_WB;
		invept_all();
		kprintf("[host] VM2 EPT violation (%s GPA %x): mapping host page %x there\n",
			(q & 2) ? "write" : "read", gpa, (u64)page);
		*resume = true;		/* re-execute the access, now it succeeds */
		return true;
	}
	case EXIT_VMCALL:
		if (r->rax == HCALL_REPORT) {
			kprintf("[host] VM2 hypercall: guest reports %u\n", r->rbx & 0xffff);
			advance_rip();
			*resume = true;
			return true;
		}
		return false;
	case EXIT_HLT:
		kprintf("[host] VM2 HLT: VM finished, VMXOFF\n");
		devirt_rsp = sep_host_rsp;	/* back into vmx_run_separate */
		devirt_rflags = 0x2;
		vmxoff();
		write_cr4(read_cr4() & ~CR4_VMXE);
		*resume = false;
		return true;
	}
	return false;	/* IO etc.: shared handlers */
}

static void run_separate_guest(void)
{
	kprintf("\n[host] === phase 2: a separate real-mode VM ===\n");
	phase = 2;
	guest_tag = "[vm2]   ";
	guest_line_start = true;
	for (int i = 0; i < EXIT_REASON_MAX; i++) exit_count[i] = 0;

	u64 p2 = rdmsr(MSR_IA32_VMX_PROCBASED_CTLS2);
	if (!((p2 >> 32) & CPU2_UNRESTRICTED_GUEST))
		panic("no unrestricted-guest support: real mode guests need it");

	/* Guest RAM: GPA 0x0000-0xffff -> vm2_ram. Program at GPA 0x1000. */
	__builtin_memcpy(vm2_ram[1], guest16_start, guest16_end - guest16_start);
	ept2_pml4[0] = (u64)ept2_pdpt | EPT_RWX;
	ept2_pdpt[0] = (u64)ept2_pd | EPT_RWX;
	ept2_pd[0]   = (u64)ept2_pt | EPT_RWX;
	for (int i = 0; i < VM2_RAM_PAGES; i++)
		ept2_pt[i] = (u64)vm2_ram[i] | EPT_RWX | EPT_MT_WB;
	kprintf("[host] VM2: %u bytes of code, 64 KiB RAM at host %x, nothing else mapped\n",
		(u64)(guest16_end - guest16_start), (u64)vm2_ram);

	/* VMX on again (we turned it off when leaving phase 1). */
	write_cr4(read_cr4() | CR4_VMXE);
	if (!vmxon((u64)vmxon_region))
		panic("VMXON (phase 2) failed");

	/* Reuse setup_vmcs for controls + host state, then override. */
	setup_vmcs_region(vmcs2_region);
	setup_vmcs_common((u64)ept2_pml4 | (3 << 3) | 6);

	bool true_ctls = rdmsr(MSR_IA32_VMX_BASIC) & (1ull << 55);
	vmwrite(SECONDARY_VM_EXEC_CONTROL,
		adjust(CPU2_ENABLE_EPT | CPU2_UNRESTRICTED_GUEST, MSR_IA32_VMX_PROCBASED_CTLS2));
	vmwrite(VM_ENTRY_CONTROLS, adjust(ENTRY_LOAD_EFER,
		true_ctls ? MSR_IA32_VMX_TRUE_ENTRY : MSR_IA32_VMX_ENTRY_CTLS));
	vmwrite(VM_EXIT_CONTROLS, adjust(EXIT_HOST_ADDR_SPACE_SIZE | EXIT_LOAD_EFER,
		true_ctls ? MSR_IA32_VMX_TRUE_EXIT : MSR_IA32_VMX_EXIT_CTLS));
	vmwrite(HOST_IA32_EFER, rdmsr(MSR_EFER));
	vmwrite(GUEST_IA32_EFER, 0);

	/* Real-mode guest state, like a CPU right after the BIOS jumps to us. */
	u64 cr0_fixed0 = rdmsr(MSR_IA32_VMX_CR0_FIXED0) & ~0x80000001ull;	/* PE, PG may be 0 */
	vmwrite(GUEST_CR0, cr0_fixed0 | 0x10);	/* NE (required) | ET */
	vmwrite(GUEST_CR3, 0);
	vmwrite(GUEST_CR4, rdmsr(MSR_IA32_VMX_CR4_FIXED0));	/* just VMXE */
	vmwrite(CR0_READ_SHADOW, 0x10);
	vmwrite(CR4_READ_SHADOW, 0);
	vmwrite(GUEST_RFLAGS, 0x2);
	vmwrite(GUEST_RIP, 0x1000);
	vmwrite(GUEST_RSP, 0x8000);
	static const u32 sel[] = { GUEST_ES_SELECTOR, GUEST_CS_SELECTOR, GUEST_SS_SELECTOR,
				   GUEST_DS_SELECTOR, GUEST_FS_SELECTOR, GUEST_GS_SELECTOR };
	for (int i = 0; i < 6; i++) {	/* selector 0, base 0, 64 KiB limit */
		vmwrite(sel[i], 0);
		vmwrite(sel[i] - GUEST_ES_SELECTOR + GUEST_ES_BASE, 0);
		vmwrite(sel[i] - GUEST_ES_SELECTOR + GUEST_ES_LIMIT, 0xffff);
		vmwrite(sel[i] - GUEST_ES_SELECTOR + GUEST_ES_AR, i == 1 ? 0x9b : 0x93);
	}
	vmwrite(GUEST_LDTR_SELECTOR, 0);
	vmwrite(GUEST_LDTR_AR, 0x10000);	/* unusable: a zeroed AR would mean "usable, type 0" */
	vmwrite(GUEST_DR7, 0x400);
	vmwrite(GUEST_TR_SELECTOR, 0);
	vmwrite(GUEST_TR_BASE, 0);
	vmwrite(GUEST_TR_LIMIT, 0xff);
	vmwrite(GUEST_TR_AR, 0x8b);
	vmwrite(GUEST_GDTR_BASE, 0);
	vmwrite(GUEST_GDTR_LIMIT, 0xffff);
	vmwrite(GUEST_IDTR_BASE, 0);
	vmwrite(GUEST_IDTR_LIMIT, 0x3ff);	/* real-mode IVT (empty here) */

	if (vmx_run_separate() != 0) {
		kprintf("[host] VMLAUNCH failed, VM-instruction error %u\n", vmread(VM_INSTRUCTION_ERROR));
		panic("vmlaunch (phase 2)");
	}
	kprintf("[host] VM2 is gone. The page we demand-mapped at GPA b8000 holds \"%c%c\"\n",
		vm2_spare[0][0], vm2_spare[0][1]);
}

bool vmexit_handler(struct guest_regs *r)
{
	u32 reason = vmread(VM_EXIT_REASON) & 0xffff;
	if (reason < EXIT_REASON_MAX) exit_count[reason]++;

	if (phase == 2) {
		bool r2;
		if (vm2_exit(reason, r, &r2))
			return r2;
	}

	switch (reason) {
	case EXIT_CPUID:		handle_cpuid(r); return true;
	case EXIT_IO:			handle_io(r); return true;
	case EXIT_EPT_VIOLATION:	handle_ept_violation(); return true;
	case EXIT_MTF:			handle_mtf(); return true;
	case EXIT_VMCALL:		return handle_vmcall(r);
	case EXIT_HLT:
		kprintf("[host] guest executed HLT (with IF=0 it would hang forever natively); skipping\n");
		advance_rip();
		return true;
	case EXIT_EXCEPTION_NMI: {
		u32 info = vmread(VM_EXIT_INTR_INFO);
		kprintf("[host] guest exception vector %u at RIP %x, qual %x\n",
			(u64)(info & 0xff), vmread(GUEST_RIP), vmread(EXIT_QUALIFICATION));
		panic("guest exception");
	}
	default:
		kprintf("[host] unhandled exit reason %u (raw %x), qual %x, RIP %x\n",
			(u64)reason, vmread(VM_EXIT_REASON), vmread(EXIT_QUALIFICATION), vmread(GUEST_RIP));
		panic("unhandled VM exit");
	}
}

void vmresume_failed(void)
{
	kprintf("[host] VMRESUME failed, VM-instruction error %u\n", vmread(VM_INSTRUCTION_ERROR));
	panic("vmresume");
}

/* ------------------------------------------------------------------ */
/* What runs as the guest                                              */

static void print_vendor(u32 leaf)
{
	u32 a, s[3];
	cpuid(leaf, &a, &s[0], &s[1], &s[2]);
	char v[13];
	__builtin_memcpy(v, &s[0], 4);
	__builtin_memcpy(v + 4, leaf ? &s[1] : &s[2], 4);	/* leaf 0 order is EBX,EDX,ECX */
	__builtin_memcpy(v + 8, leaf ? &s[2] : &s[1], 4);
	v[12] = 0;
	for (int i = 0; i < 12; i++) if (v[i] < 32 || v[i] > 126) v[i] = '.';
	kprintf("cpuid(%x) vendor = \"%s\"\n", (u64)leaf, v);
}

static u64 hypercall(u64 nr, u64 arg)
{
	__asm__ volatile("vmcall" : "+a"(nr) : "b"(arg) : "memory");
	return nr;
}

static void guest_demo(void)
{
	u32 a, b, c, d;
	kprintf("hello from VMX non-root mode; this is still kmain()\n");
	print_vendor(0);
	print_vendor(0x40000000);
	cpuid(1, &a, &b, &c, &d);
	kprintf("cpuid(1).ecx: hypervisor bit = %u, VMX bit = %u\n", (u64)((c >> 31) & 1), (u64)((c >> 5) & 1));
	kprintf("hypercall ping -> %x\n", hypercall(HCALL_PING, 42));
	kprintf("executing HLT with interrupts off...\n");
	__asm__ volatile("hlt");
	kprintf("...and we are still alive\n");
	u8 r = watched_page[0];
	kprintf("read watched page: %x (no exit, page is readable)\n", (u64)r);
	kprintf("writing 3 bytes to the watched page\n");
	watched_page[0] = 0x41;
	watched_page[1] = 0x42;
	watched_page[2] = 0x43;
	kprintf("asking the hypervisor to leave\n");
	hypercall(HCALL_DEVIRT, 0);
}

/* ------------------------------------------------------------------ */

static void setup_tss(void)
{
	u64 base = (u64)&tss, limit = sizeof(tss) - 1;
	tss.iomap = sizeof(tss);
	gdt[4] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ull << 40) |
		 (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
	gdt[5] = base >> 32;
	__asm__ volatile("lgdt %0; ltr %w1" :: "m"(gdt_desc), "r"(0x20));
}

static void print_stats(const char *tag);

void kmain(void)
{
	kprintf("\n[host] minivisor: bare-metal VT-x demo\n");
	setup_tss();
	enable_vmx();
	u64 eptp = build_ept();
	setup_vmcs(eptp);

	kprintf("[host] VMLAUNCH: the code calling us becomes the guest\n");
	if (vmx_subvert() != 0) {
		kprintf("[host] VMLAUNCH failed, VM-instruction error %u\n", vmread(VM_INSTRUCTION_ERROR));
		panic("vmlaunch");
	}

	guest_demo();	/* everything in here runs in VMX non-root mode */

	/* Back to bare metal: the same queries now hit the real CPU. */
	kprintf("[native] devirtualized. Same queries, no hypervisor:\n[native] ");
	print_vendor(0x40000000);
	kprintf("[native] watched page now: %c%c%c\n", watched_page[0], watched_page[1], watched_page[2]);
	print_stats("[native]");
	run_separate_guest();
	print_stats("[host]");
	kprintf("[host] done.\n");
	shutdown();
}

static void print_stats(const char *tag)
{
	kprintf("%s VM exits handled:", tag);
	static const char *names[EXIT_REASON_MAX] = {
		[EXIT_CPUID] = "CPUID", [EXIT_HLT] = "HLT", [EXIT_VMCALL] = "VMCALL",
		[EXIT_IO] = "IO", [EXIT_MTF] = "MTF", [EXIT_EPT_VIOLATION] = "EPT",
	};
	for (int i = 0; i < EXIT_REASON_MAX; i++)
		if (exit_count[i])
			kprintf(" %s=%u", names[i] ? names[i] : "?", exit_count[i]);
	kprintf("\n");
}
