# minivisor

A small bare-metal Intel VT-x hypervisor that runs in [Bochs](https://github.com/bochs-emu/Bochs),
written to study how hypervisors work. It is about 600 lines of C plus three
small assembly files, with no OS underneath.

It runs in two phases:

1. **Blue pill.** The code that starts the hypervisor keeps running, now as
   its guest. It shows CPUID spoofing, a hypercall, trapped `HLT` and port I/O,
   EPT write-watching with monitor-trap-flag re-protection, then `VMXOFF` and
   back to bare metal.
2. **Separate VM.** A 16-bit real-mode program with its own 64 KiB of RAM and
   its own EPT (unrestricted guest). It prints, reports a result through a
   hypercall, touches unmapped memory that the host maps on demand, and ends
   with `HLT`.

See [`sample-output.txt`](sample-output.txt) for a full run, and
[`STUDY.md`](STUDY.md) for a step-by-step map of the same lifecycle in
[Hypervisor From Scratch](https://github.com/SinaKarvandi/Hypervisor-From-Scratch)
and [ksm](https://github.com/asamy/ksm).

## Build and run

Needs clang, ld.lld, llvm-objcopy, and Bochs built with VMX:

```sh
# Bochs (once)
./configure --prefix=$HOME/.local/opt/bochs-3.1 --enable-x86-64 --enable-vmx=2 \
    --enable-cpu-level=6 --enable-debugger --disable-debugger-gui --with-nogui
make && make install

# minivisor
cd minivisor
make run              # BOCHS_HOME=... to use another Bochs install
./run.sh debug        # start in the Bochs debugger
```

The build cross-compiles with `--target=x86_64-unknown-none-elf`, so it works
on any host, including arm64. Bochs emulates VMX and EPT in software, which
QEMU's TCG does not.

## Layout

| File | What it is |
|---|---|
| `minivisor/boot.S` | boot sector → protected mode → long mode, 4 GiB identity map |
| `minivisor/main.c` | VMXON, VMCS, EPT, exit handlers, both phases |
| `minivisor/entry.S` | VMLAUNCH helpers and the VM-exit stub |
| `minivisor/guest16.S` | the real-mode guest of phase 2 |
| `minivisor/vmx.h` | VMCS encodings, MSRs, VMX instruction wrappers |

## Credits

minivisor is new code, but it stands on the work of others:

- **[Hypervisor From Scratch](https://github.com/SinaKarvandi/Hypervisor-From-Scratch)**
  by Sina Karvandi and contributors (MIT), with the
  [tutorial series](https://rayanfam.com/tutorials/) at rayanfam.com. Its
  structure (VMXON → VMCS → launch → exits → EPT → hooks) is the structure
  minivisor follows, and its MTF-based EPT hooks inspired the page watcher.
- **[ksm](https://github.com/asamy/ksm)** by Ahmed Samy (GPLv2). The
  subvert-the-running-system launch and the guest register frame in
  `entry.S` follow the same approach as its `vmx.S`. No ksm code is copied.
  That frame layout is the common convention also used by KVM.
- **[Bochs](https://github.com/bochs-emu/Bochs)** by the Bochs project (LGPL),
  whose software VMX/EPT emulation makes this runnable on any host, and whose
  logs name the exact VM-entry check that failed. Used as a tool, not
  redistributed.
- **Intel® 64 and IA-32 Architectures Software Developer's Manual**, Vol. 3C
  (VMX chapters and appendices), the reference for every VMCS encoding,
  control bit and exit reason here.
- **Joanna Rutkowska**, whose 2006 *Blue Pill* gave the name to virtualizing a
  running system on the fly.
- Written with help from Claude (Anthropic).

## License

MIT, see [LICENSE](LICENSE).
