/*
 * vmx.h - VMX constants and instruction wrappers.
 * Encodings: Intel SDM Vol. 3C, Appendix B. Exit reasons: Appendix C.
 * Compare: ksm/vmx.h, HFS Part 5 VMX.h.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* ---- MSRs ---- */
#define MSR_IA32_FEATURE_CONTROL	0x03a
#define MSR_IA32_VMX_BASIC		0x480
#define MSR_IA32_VMX_PINBASED_CTLS	0x481
#define MSR_IA32_VMX_PROCBASED_CTLS	0x482
#define MSR_IA32_VMX_EXIT_CTLS		0x483
#define MSR_IA32_VMX_ENTRY_CTLS		0x484
#define MSR_IA32_VMX_CR0_FIXED0		0x486
#define MSR_IA32_VMX_CR0_FIXED1		0x487
#define MSR_IA32_VMX_CR4_FIXED0		0x488
#define MSR_IA32_VMX_CR4_FIXED1		0x489
#define MSR_IA32_VMX_PROCBASED_CTLS2	0x48b
#define MSR_IA32_VMX_EPT_VPID_CAP	0x48c
#define MSR_IA32_VMX_TRUE_PINBASED	0x48d
#define MSR_IA32_VMX_TRUE_PROCBASED	0x48e
#define MSR_IA32_VMX_TRUE_EXIT		0x48f
#define MSR_IA32_VMX_TRUE_ENTRY		0x490

#define FEATURE_CONTROL_LOCKED		(1u << 0)
#define FEATURE_CONTROL_VMXON_OUTSIDE_SMX (1u << 2)
#define CR4_VMXE			(1u << 13)

/* ---- VM-execution controls ---- */
#define CPU_HLT_EXITING			(1u << 7)
#define CPU_MTF				(1u << 27)	/* monitor trap flag */
#define CPU_USE_IO_BITMAPS		(1u << 25)
#define CPU_USE_MSR_BITMAPS		(1u << 28)
#define CPU_SECONDARY			(1u << 31)
#define CPU2_ENABLE_EPT			(1u << 1)
#define CPU2_UNRESTRICTED_GUEST		(1u << 7)
#define EXIT_LOAD_EFER			(1u << 21)
#define ENTRY_LOAD_EFER			(1u << 15)
#define MSR_EFER			0xc0000080
#define EXIT_HOST_ADDR_SPACE_SIZE	(1u << 9)
#define ENTRY_IA32E_MODE_GUEST		(1u << 9)

/* ---- VMCS field encodings ---- */
enum vmcs_field {
	/* 16-bit */
	GUEST_ES_SELECTOR = 0x0800, GUEST_CS_SELECTOR = 0x0802,
	GUEST_SS_SELECTOR = 0x0804, GUEST_DS_SELECTOR = 0x0806,
	GUEST_FS_SELECTOR = 0x0808, GUEST_GS_SELECTOR = 0x080a,
	GUEST_LDTR_SELECTOR = 0x080c, GUEST_TR_SELECTOR = 0x080e,
	HOST_ES_SELECTOR = 0x0c00, HOST_CS_SELECTOR = 0x0c02,
	HOST_SS_SELECTOR = 0x0c04, HOST_DS_SELECTOR = 0x0c06,
	HOST_FS_SELECTOR = 0x0c08, HOST_GS_SELECTOR = 0x0c0a,
	HOST_TR_SELECTOR = 0x0c0c,
	/* 64-bit */
	IO_BITMAP_A = 0x2000, IO_BITMAP_B = 0x2002, MSR_BITMAP = 0x2004,
	EPT_POINTER = 0x201a,
	GUEST_PHYSICAL_ADDRESS = 0x2400,
	VMCS_LINK_POINTER = 0x2800, GUEST_IA32_DEBUGCTL = 0x2802,
	GUEST_IA32_EFER = 0x2806, HOST_IA32_EFER = 0x2c02,
	/* 32-bit */
	PIN_BASED_VM_EXEC_CONTROL = 0x4000, CPU_BASED_VM_EXEC_CONTROL = 0x4002,
	EXCEPTION_BITMAP = 0x4004, CR3_TARGET_COUNT = 0x400a,
	VM_EXIT_CONTROLS = 0x400c, VM_EXIT_MSR_STORE_COUNT = 0x400e,
	VM_EXIT_MSR_LOAD_COUNT = 0x4010, VM_ENTRY_CONTROLS = 0x4012,
	VM_ENTRY_MSR_LOAD_COUNT = 0x4014, VM_ENTRY_INTR_INFO = 0x4016,
	SECONDARY_VM_EXEC_CONTROL = 0x401e,
	VM_INSTRUCTION_ERROR = 0x4400, VM_EXIT_REASON = 0x4402,
	VM_EXIT_INTR_INFO = 0x4404, VM_EXIT_INSTRUCTION_LEN = 0x440c,
	GUEST_ES_LIMIT = 0x4800, GUEST_CS_LIMIT = 0x4802,
	GUEST_SS_LIMIT = 0x4804, GUEST_DS_LIMIT = 0x4806,
	GUEST_FS_LIMIT = 0x4808, GUEST_GS_LIMIT = 0x480a,
	GUEST_LDTR_LIMIT = 0x480c, GUEST_TR_LIMIT = 0x480e,
	GUEST_GDTR_LIMIT = 0x4810, GUEST_IDTR_LIMIT = 0x4812,
	GUEST_ES_AR = 0x4814, GUEST_CS_AR = 0x4816, GUEST_SS_AR = 0x4818,
	GUEST_DS_AR = 0x481a, GUEST_FS_AR = 0x481c, GUEST_GS_AR = 0x481e,
	GUEST_LDTR_AR = 0x4820, GUEST_TR_AR = 0x4822,
	GUEST_INTERRUPTIBILITY = 0x4824, GUEST_ACTIVITY_STATE = 0x4826,
	GUEST_SYSENTER_CS = 0x482a, HOST_SYSENTER_CS = 0x4c00,
	/* natural width */
	CR0_GUEST_HOST_MASK = 0x6000, CR4_GUEST_HOST_MASK = 0x6002,
	CR0_READ_SHADOW = 0x6004, CR4_READ_SHADOW = 0x6006,
	EXIT_QUALIFICATION = 0x6400, GUEST_LINEAR_ADDRESS = 0x640a,
	GUEST_CR0 = 0x6800, GUEST_CR3 = 0x6802, GUEST_CR4 = 0x6804,
	GUEST_ES_BASE = 0x6806, GUEST_CS_BASE = 0x6808, GUEST_SS_BASE = 0x680a,
	GUEST_DS_BASE = 0x680c, GUEST_FS_BASE = 0x680e, GUEST_GS_BASE = 0x6810,
	GUEST_LDTR_BASE = 0x6812, GUEST_TR_BASE = 0x6814,
	GUEST_GDTR_BASE = 0x6816, GUEST_IDTR_BASE = 0x6818,
	GUEST_DR7 = 0x681a, GUEST_RSP = 0x681c, GUEST_RIP = 0x681e,
	GUEST_RFLAGS = 0x6820, GUEST_PENDING_DBG = 0x6822,
	GUEST_SYSENTER_ESP = 0x6824, GUEST_SYSENTER_EIP = 0x6826,
	HOST_CR0 = 0x6c00, HOST_CR3 = 0x6c02, HOST_CR4 = 0x6c04,
	HOST_FS_BASE = 0x6c06, HOST_GS_BASE = 0x6c08, HOST_TR_BASE = 0x6c0a,
	HOST_GDTR_BASE = 0x6c0c, HOST_IDTR_BASE = 0x6c0e,
	HOST_SYSENTER_ESP = 0x6c10, HOST_SYSENTER_EIP = 0x6c12,
	HOST_RSP = 0x6c14, HOST_RIP = 0x6c16,
};

/* ---- Basic exit reasons ---- */
enum {
	EXIT_EXCEPTION_NMI = 0, EXIT_EXTERNAL_INT = 1, EXIT_TRIPLE_FAULT = 2,
	EXIT_CPUID = 10, EXIT_HLT = 12, EXIT_VMCALL = 18, EXIT_CR_ACCESS = 28,
	EXIT_IO = 30, EXIT_RDMSR = 31, EXIT_WRMSR = 32,
	EXIT_INVALID_GUEST_STATE = 33, EXIT_MTF = 37,
	EXIT_EPT_VIOLATION = 48, EXIT_EPT_MISCONFIG = 49,
	EXIT_REASON_MAX = 70,
};

/* ---- Plain x86 helpers ---- */
static inline u64 rdmsr(u32 msr)
{
	u32 lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((u64)hi << 32) | lo;
}
static inline void wrmsr(u32 msr, u64 v)
{
	__asm__ volatile("wrmsr" :: "c"(msr), "a"((u32)v), "d"((u32)(v >> 32)));
}
#define DEF_CR(n) \
	static inline u64 read_cr##n(void) { u64 v; __asm__ volatile("mov %%cr" #n ", %0" : "=r"(v)); return v; } \
	static inline void write_cr##n(u64 v) { __asm__ volatile("mov %0, %%cr" #n :: "r"(v) : "memory"); }
DEF_CR(0)
DEF_CR(3)
DEF_CR(4)
static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline u64 read_rflags(void) { u64 f; __asm__ volatile("pushfq; pop %0" : "=r"(f)); return f; }
static inline void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
	__asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

/* ---- VMX instructions. All return true on success (CF=0 and ZF=0). ---- */
static inline bool vmxon(u64 pa)
{
	u8 bad;
	__asm__ volatile("vmxon %1; setna %0" : "=qm"(bad) : "m"(pa) : "cc", "memory");
	return !bad;
}
static inline void vmxoff(void) { __asm__ volatile("vmxoff" ::: "cc", "memory"); }
static inline bool vmclear(u64 pa)
{
	u8 bad;
	__asm__ volatile("vmclear %1; setna %0" : "=qm"(bad) : "m"(pa) : "cc", "memory");
	return !bad;
}
static inline bool vmptrld(u64 pa)
{
	u8 bad;
	__asm__ volatile("vmptrld %1; setna %0" : "=qm"(bad) : "m"(pa) : "cc", "memory");
	return !bad;
}
static inline u64 vmread(u64 field)
{
	u64 v;
	__asm__ volatile("vmread %1, %0" : "=r"(v) : "r"(field) : "cc");
	return v;
}
static inline bool vmwrite(u64 field, u64 v)
{
	u8 bad;
	__asm__ volatile("vmwrite %2, %1; setna %0" : "=qm"(bad) : "r"(field), "rm"(v) : "cc");
	return !bad;
}
static inline void invept_all(void)
{
	struct { u64 eptp, rsvd; } desc = { 0, 0 };
	__asm__ volatile("invept %0, %1" :: "m"(desc), "r"(2ull) : "cc", "memory");
}
