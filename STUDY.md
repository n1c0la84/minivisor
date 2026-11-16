# Hypervisor study: HFS, ksm, minivisor

Three codebases, one lifecycle. Read them side by side.

| | Hypervisor-From-Scratch (HFS) | ksm | minivisor (ours) |
|---|---|---|---|
| Where it runs | Windows kernel driver (WDK, MSVC/MASM) | Windows or Linux kernel module | bare metal, booted by BIOS |
| Style | 8 incremental tutorial parts | one compact, real project | ~600 lines, single-file C + two asm files |
| Runs here? | no (Windows, needs real/nested VT-x) | Linux 3.16–4.8 inside Bochs (not done yet) | **yes, in Bochs on this arm64 laptop** |

This laptop is a Snapdragon (arm64), so none of this runs natively. Bochs
emulates an Intel CPU *including VMX and EPT* in software (`--enable-vmx=2`),
QEMU's TCG does not. Bochs 3.1 is installed in `~/.local/opt/bochs-3.1`
(source in `third_party/Bochs`).

```
cd minivisor && make run       # build + run: phase 1 blue pill, phase 2 separate VM
./run.sh debug                 # same, but start in the Bochs debugger
```

## The lifecycle, step by step

### 1. Is VT-x available and allowed?
`CPUID.1:ECX[5]` = VMX. `IA32_FEATURE_CONTROL` (MSR 0x3A) must have lock +
"VMXON outside SMX"; firmware normally sets it. CR0/CR4 must respect
`IA32_VMX_CR{0,4}_FIXED{0,1}` (e.g. CR0.NE must be 1), then set CR4.VMXE.

- HFS: Part 2 `Source.c` (`IsVmxSupported`, `AsmEnableVmxOperation`)
- ksm: `ksm.c:106` `__ksm_init_cpu`
- minivisor: `main.c` `enable_vmx()`

### 2. VMXON
A 4 KiB aligned region whose first dword is the VMCS revision id from
`IA32_VMX_BASIC`. `VMXON <phys addr>` puts the CPU in VMX root mode. Every
VMX instruction reports failure via CF (VMfailInvalid) or ZF (VMfailValid,
error number in `VM_INSTRUCTION_ERROR`).

- HFS: Part 2/3 `AllocateVmxonRegion`
- minivisor: `vmx.h` wrappers use `setna` to catch both CF and ZF

### 3. The VMCS
One per logical CPU. `VMCLEAR` then `VMPTRLD` makes it "current"; from then
on `VMREAD`/`VMWRITE` take a *field encoding*, never a memory offset (the
layout is secret and differs per CPU). Three groups of fields:

- **Controls**: what causes exits. Pin-based, primary/secondary
  processor-based, exit, entry. Never write raw values: each control MSR says
  which bits *must* be 1 (low dword) and which *may* be 1 (high dword).
  That is `AdjustControls` (HFS), `adjust_ctl_val` (ksm `vcpu.c:474`),
  `adjust()` (minivisor). Use the `TRUE_*` MSRs when `VMX_BASIC[55]` is set.
- **Host state**: loaded on every VM exit. Most importantly `HOST_RIP` (exit
  stub) and `HOST_RSP` (a private VMM stack).
- **Guest state**: loaded on VM entry. Segments are spelled out as
  selector/base/limit/access-rights, and the entry checks are strict (TR must
  be a busy TSS, LDTR unusable, CS.L=1 for 64-bit...). HFS Part 5 ships
  `VMCS-Checks/CheckGuestVmcsFieldsForVmEntry.c`, which re-implements the SDM
  checks. Keep it open when an entry fails with exit reason 33.

- HFS: Part 5 `VMX.c` `SetupVmcs`
- ksm: `vcpu.c:481` `vcpu_run` (EPTP at :706, GUEST_RIP :835, HOST_RIP :866, launch :877)
- minivisor: `setup_vmcs()`

### 4. EPT: second-level address translation
Guest-physical → host-physical, a 4-level table like normal paging but with
R/W/X bits per entry and a memory type in the leaves. A "thin" hypervisor
uses an **identity map** (GPA == HPA) and only edits permissions to *watch*
memory. Large (2 MiB) pages keep it small; to watch one 4 KiB page, split its
2 MiB entry into a page table. After changing entries: `INVEPT`.

- HFS: Part 4 (concepts), Part 7 `Ept.c` (identity map from MTRRs), Part 8 `Ept.c:171` `EptSplitLargePage`
- ksm: `vcpu.c` `ept_alloc_page`, `setup_pml4`; memory types from MTRRs in `ksm_init`
- minivisor: `build_ept()`

### 5. VMLAUNCH: two ways to use it
- **Run separate guest code** (HFS Part 5, and what KVM/QEMU do): the guest
  gets its own state and memory. minivisor phase 2 (`run_separate_guest()`,
  `guest16.S`) runs a 16-bit real-mode program in 64 KiB of guest-physical
  RAM backed by its own EPT. That needs *unrestricted guest* (secondary
  control bit 7), otherwise the guest must start in paged protected mode. Unmapped
  GPAs cause EPT violations, which the host answers by mapping a page on demand.
  `HLT` ends the VM.
- **Blue pill / subvert the running system** (HFS Part 6+, ksm, minivisor):
  save registers, write GUEST_RSP = current RSP and GUEST_RIP = a label just
  after `VMLAUNCH`, then launch. If it succeeds, execution *continues at that
  label*, still the same OS code, now in VMX non-root mode. Done per CPU
  (DPC on Windows, `on_each_cpu` on Linux: ksm `DEFINE_DPC`/`CALL_DPC`).

- HFS: Part 6 `VMXState.asm` (`AsmVmxSaveState` / `AsmVmxRestoreState`)
- ksm: `vmx.S:232` `__vmx_vminit` → `vcpu_run` → returns at `do_resume`
- minivisor: `entry.S` `vmx_subvert`

### 6. VM exits
CPU switches to host state, `RIP = HOST_RIP`, and **guest GPRs are still in
the registers**. The stub pushes them into a frame (rax, rcx, rdx, rbx, rsp
slot, rbp, rsi, rdi, r8–r15; ksm and minivisor use exactly this layout),
calls C, pops, `VMRESUME`. The handler reads `VM_EXIT_REASON` and
`EXIT_QUALIFICATION` and usually advances `GUEST_RIP` by
`VM_EXIT_INSTRUCTION_LEN`.

Some exits are unconditional (CPUID, INVD, XSETBV, every VMX instruction),
the rest are opt-in through controls and bitmaps (HLT, I/O bitmap, MSR bitmap,
exception bitmap, CR masks).

- HFS: Part 6 `VMExitHandler.asm` + `MainVmexitHandler`; Part 8 `Exit.c`
- ksm: `vmx.S:260` `__vmx_entrypoint`, `exit.c:2665` `vcpu_handle_exit`, table `g_handlers` at `exit.c:2555`
- minivisor: `entry.S` `vmexit_stub`, `main.c` `vmexit_handler`

### 7. EPT hooks: the "magic"
Remove a permission → EPT violation exit with the GPA and the access type.
To let the access through *once*: restore the permission, set the **monitor
trap flag**, resume; after one guest instruction an MTF exit fires and you
remove the permission again. Hidden execute hooks go further: EPT points
execute fetches at a modified copy of the page and reads/writes at the
original, so integrity checks see clean code.

- HFS: Part 7 (monitoring), Part 8 `Ept.c:441-641` (`EptHandlePageHookExit`, `EptHandleMonitorTrapFlag`, `EptPerformPageHook`), `HiddenHooks.c`, `SyscallHook.c`
- ksm: `epage.c` uses *multiple EPTPs* and `VMFUNC` EPTP switching (or `#VE`) instead of MTF, which is faster because there's no exit per access
- minivisor: `handle_ept_violation()` + `handle_mtf()`

### 8. Leaving: VMXOFF
A hypercall (`VMCALL` with a magic number) asks the host to quit. The host
reads the guest's RIP/RSP/RFLAGS, executes `VMXOFF`, restores guest
GDTR/IDTR/CR3 (a VM exit forces their limits to 0xFFFF), and jumps to the
guest's next instruction on the guest stack.

- HFS: Part 6/8 `Vmcall.c`, `AsmVmxoffAndRestoreState`
- ksm: `ksm.c:277` `__ksm_exit_cpu`, `vcpu_handle_vmcall` `exit.c:966`
- minivisor: `handle_vmcall(HCALL_DEVIRT)` + `.Ldevirt` in `entry.S`

## Suggested reading order
1. Run minivisor and read `main.c` top to bottom while reading the output
   (one exit per line).
2. HFS Parts 2 → 3 → 5 → 6, reading the blog posts; the code mirrors steps 1–6.
3. ksm `ksm.c`, `vcpu.c:vcpu_run`, `vmx.S`, `exit.c:vcpu_handle_exit`; the
   same thing written by someone who also had to support real OSes, SMP,
   MTRRs and nesting.
4. HFS Parts 7–8 and ksm `epage.c` for the hooking techniques.
5. Intel SDM Vol. 3C chapters 24–28 (VMCS, entry checks, exits, EPT) is the
   reference all three are written against.

## Exercises in minivisor (in rough order)
- Trap `RDMSR` of `IA32_FEATURE_CONTROL` through the MSR bitmap and lie about it.
- Break a VM entry on purpose (e.g. `GUEST_TR_AR = 0x89`) and read exit reason 33 / qualification.
- Use the Bochs debugger: `./run.sh debug`, then `vb 0x08:<rip>`, `c`, and
  `info vmcs`... see `help` for the VMX-related commands Bochs exposes.
- Replace MTF with a second EPTP + VMFUNC like ksm (Bochs Haswell model has VMFUNC; check `IA32_VMX_PROCBASED_CTLS2`).
- Execute-only hook: make a page `X` only, redirect reads to a decoy page.
- Phase 2: make the guest switch itself to protected mode (needs a GDT in guest RAM).
- Phase 2: emulate a device: leave a GPA unmapped and decode the faulting `mov` instead of mapping RAM (how QEMU does MMIO).
- Inject an exception into the guest (`VM_ENTRY_INTR_INFO`), as in HFS Part 8 `EventInjectAndExceptionBitmap`.

## Next: ksm in Bochs (not done yet)
ksm needs a Linux 4.x x86-64 kernel inside Bochs plus a cross-built module.
Plan: Debian stretch (4.9) kernel + headers, cross-compile with a matching
older GCC/clang, small initramfs with busybox, `insmod ksmlinux.ko`.
Expect Bochs to boot Linux in a few minutes.
