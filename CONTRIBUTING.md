# Contributing to EmbLinkOs

EmbLinkOs is a complete x86_64 operating system built from absolute zero — custom
bootloader, kernel, filesystem, userspace, all hand-written with the goal of
understanding every line. This document describes how work is done here so the
project stays coherent and every change is something you could rebuild and
explain.

If you're returning to this after time away, or picking it up fresh, start by
reading **[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)** (the intended design
and the decisions behind it — plus per-subsystem deep dives under
`docs/architecture/`), then **[`docs/PROJECT_STATUS.md`](docs/PROJECT_STATUS.md)**
and **[`docs/TODO.md`](docs/TODO.md)** (what's actually built vs. what's left).
The repo is ground truth; the docs describe intent.

If this is a fresh clone/fork and you haven't built the toolchain yet, do
that first: **[`docs/BUILD_SETUP.md`](docs/BUILD_SETUP.md)** (cross compiler,
the newlib rebuild, dynamic linking). If you're here to build a graphical
app rather than hack on the kernel, **[`docs/EMUI_GUIDE.md`](docs/EMUI_GUIDE.md)**
is the faster on-ramp; **[`docs/EMUI_INTERNALS.md`](docs/EMUI_INTERNALS.md)**
covers extending the UI toolkit itself.

## The core principle

**The working OS is not the deliverable — understanding is.** If the goal were a
booting kernel, you'd run Linux. The point of building from scratch is to know
exactly why every line exists and be able to rebuild it. A change you can't
explain is worthless against that goal, even if it compiles and boots. So the
bar for "done" is not "it works" — it's "I understand why it works, and a test
proves it does the thing it claims."

## How a change gets made

1. **Concept first.** Understand the mechanism before writing code. If it
   touches hardware (paging, privilege levels, interrupts, TSS, MSRs) or an
   on-disk format, read the relevant **Intel SDM** section (or the EMBKFS spec)
   *first*. Cite it in comments and the PR.
2. **Justify the decision.** Don't skip the "why." At a fork, say why this
   approach over the alternatives. Design decisions get recorded in
   `docs/ARCHITECTURE.md` (or a `docs/architecture/*.md` subsystem spec for
   anything substantial), not lost in a commit body.
3. **Write it, one piece at a time.** Go slow. A subsystem is built and
   verified incrementally, not dumped in whole. Isolate the one new concept
   from the plumbing around it.
4. **Prove it, don't assume it.** See "Proving a change" below — this is the
   rule that catches the real bugs.
5. **Debug by reading, not guessing.** When it faults, read the fault: the
   exception vector, the error-code bits, CR2, CS/SS (which ring), CR3 (which
   address space). The dump tells you what broke and where. Decode it before
   changing anything.
6. **Commit at each milestone.** Small, milestone-sized commits with messages
   explaining the *why*, not just the *what*. Commit working progress before
   moving on to the next piece — a green checkpoint you can return to.

## Proving a change

A change isn't done because it compiles or prints the expected output once. It's
done when a **selftest exercises the actual invariant** — the specific thing
that would *fail* if the change were broken.

This is the rule that matters most, because the real bugs in this project have
all been tests that *passed while the thing they tested was broken*: an fd test
that returned OK while freeing blocks out from under an open file; a syscall
path that ran fine while leaving interrupts disabled. The fix in each case was a
test that asserted the invariant directly — "the bytes are still readable after
unlink," "interrupts still fire after the syscall returns" — not one that
checked "did it return without crashing."

So when you add a feature, ask: *what is the one fact that distinguishes correct
from broken?* Then write the test that checks that fact, and watch it be green
for the right reason.

Worked example: the compiler port's test does not check that `tcc` exits 0, or
that it emits a valid-looking ELF — either can be true of a broken compiler.
It **runs the produced binary and checks it returns 42**. That number can only
appear if the program was really compiled and really executed.

### Four ways a green test lies, all of which have bitten this repo

- **The binary under test was stale.** `python.elf` sat **15 hours** out of
  date, passing `test python` against a frozen copy of an old libc — green, and
  true of a binary nobody builds any more. The ports statically link
  `build/syscalls.o` via a raw path their own build systems treat as a file, not
  a prerequisite, so `make python` says *"up to date"* however much our libc
  changed. Our Makefile now forces the relink; check mkfs's `src fingerprint`
  line if you are unsure what got packed. **The kernel has the same trap for
  HEADERS:** `kernel.elf` is one big `gcc` over an explicit `.c` list with no
  `-MMD` depfiles, so editing a `.h` (e.g. bumping a `#define` in
  `spawn.h`) does **not** trigger a rebuild — `make` cheerfully reports success
  and packs the old kernel. This cost two full 35-minute v2 boots before the
  timestamps gave it away (`myos.img` *older* than the edited header). When you
  change a kernel header, `rm kernel/kernel.elf` (or `touch` a `.c`) before
  `make`; and if a result contradicts a change you *know* you made, diff the
  image mtime against the file you edited before doubting anything else.
- **Your code never ran at all.** Put a marker (a `kprintf` naming what you are
  testing) in any new selftest. If the marker does not appear, you are not
  running the kernel you just built — check *that* before doubting the code.
  A `pkill -f` pattern once matched its own shell's command line and killed two
  QEMU runs before they started; the `>` redirect left the *previous* log in
  place, so the stale output read as fresh and sent a debugging session chasing
  a result that never happened. (That same self-match trap has since fired
  **four more times** in one session — never put the process's match string in
  the killing command's own text.)
- **A green build is not a green test.** The extern-consumer pipeline
  (`ls / | tally`) shipped wedged: the pump rework landed after the last time
  `test extern` was actually run, and the post-merge check was "builds clean +
  the code is present" — both true, neither a test. When you touch a
  subsystem, re-run *that subsystem's* tests, not the ones that happen to be
  in muscle memory (the same gap let the IF=0 wake leak hide behind a sweep
  that re-ran posix/vmm/ksync but not extern/pipe).
- **The clock you're timing with is not wall time.** Under TCG with the
  desktop compositing, the guest's 100 Hz tick runs at a fraction of wall
  speed — tick-based test timeouts need ~3× wall budget, and "no output for a
  minute" is routinely *slow*, not hung (CPython's startup "freeze" and tcc's
  on-OS link both looked dead and weren't). Before diagnosing a hang, take a
  stable snapshot instead of watching silence: QEMU's monitor
  (`-monitor unix:...` + `info registers`, RIP→`addr2line`) or GDB `-nx`
  attach; and run QEMU on *copies* of the images — a timed-out run SIGTERMs
  qemu mid-write and quietly poisons `embkfs.img`, which `make` then keeps
  (newer mtime) for every later boot.

### Live tests worth knowing

At the serial prompt after boot (**one `test` per boot** — the console drops
serial input while a spawned process runs):

| Command | Proves |
|---|---|
| `test posix` | the libc/POSIX surface, incl. cwd and rename |
| `test tcc real` | the OS compiles+links+**runs** real `#include` C (`exit=42`, byte-exact stdio) |
| `test tcc tally` | the OS **rebuilds one of its own tools** from `/data/src` and A/B-runs it in the live pipeline |
| `test python` / `test git repo` | the interpreter / git (need `-cpu max`) |
| `test shell` / `test extern` / `test ctrlc` | structured pipelines / extern spawn plumbing / interruption |
| `test writestorm` | kernel-context fs writes + zero IF leaks (the Bug-26 regression trap) |

The port tests need their toolchain built — see
[docs/PORTS.md](docs/PORTS.md) and
[docs/BUILD_SETUP.md](docs/BUILD_SETUP.md#tldr--the-exact-order-on-a-fresh-machine).

## Docs are part of the change

Documentation drift is the default failure mode. Update, in the same change:

- **`docs/PROJECT_STATUS.md`** — when a phase completes or the build state changes.
- **`docs/ARCHITECTURE.md`** (and the relevant `docs/architecture/*.md` spec,
  if one exists for that subsystem) — when a design decision lands, or a
  planned item (🎯) becomes built (✅).
- **`docs/TODO.md`** — add gaps the change leaves behind; remove items it closes.
- **`docs/EMUI_GUIDE.md`** / **`docs/EMUI_INTERNALS.md`** — when the UI
  toolkit itself changes: a new component or macro goes in the guide's
  reference table; a new layer, mechanism, or internal invariant goes in
  the internals doc. A component that isn't in the guide is a component
  nobody but you knows how to use.

Naming what a change *doesn't* handle is part of the discipline. A known,
tracked limitation is fine; a silent one is a trap for whoever hits it next.

## Pull requests

PRs use the template in `.github/pull_request_template.md`, which prompts for
exactly these things: what changed, how it was proven, SDM/spec references for
hardware changes, docs synced, and gaps left behind. Fill it honestly — the
"how it was proven" section is the important one.

## Project conventions

- **Target:** x86_64 first, ARM64 later. SMP is built and the default boot
  config runs multi-core; keep new code SMP-safe (avoid hidden global
  mutable state; prefer passing state explicitly over stashing it in a
  global, and lock anything shared across cores — a lockless page-table
  walk and an unlocked framebuffer dirty-rect accumulator have both been
  real, live SMP bugs in this codebase, not hypothetical ones).
- **Kernel:** higher half at `0xFFFFFFFF80100000`. Boot is a custom two-stage
  BIOS bootloader (UEFI is on `docs/TODO.md`).
- **Diverge from Unix only with a concrete technical justification** (see
  `docs/ARCHITECTURE.md` §3). Bless the clean native primitive; provide the
  compatible one as an opt-in layer.
- **Formatting:** `%lu`/`%lx` for 64-bit in `kprintf`, `%08X` (cast) for 32-bit.
- **No `-Wall` in build flags** (it fights the packed-struct access the on-disk
  formats need); instead, `memcpy` into aligned locals rather than dereferencing
  packed members directly.

## Build & run

- **Environment:** Ubuntu, `x86_64-elf` cross compiler (`/usr/local/cross/bin`),
  a C99-enabled newlib rebuild (`NEWLIB_PREFIX`), NASM, QEMU, GDB, Python 3.
  First-time setup for all of this: [`docs/BUILD_SETUP.md`](docs/BUILD_SETUP.md).
- **Build the kernel + bootloader:** `make`
- **Build userland + the UI toolkit + pack a disk image:** `make embkfs.img`
  (every app in `user/bin/`, the shared `libembk.so` toolkit, and `font.ttf`,
  packed into an EMBKFS image via `tools/embkfs_mkfs/mkfs_embkfs.py`).
- **Run — kernel-only variants** (no userland disk image needed):
  `make run` (base), `make run-smp` (`-smp 4`), `make run-bigmem`,
  `make run-kvm` (if `/dev/kvm` is available), `make run-ahci`, `make run-fat`,
  `make run-all` (FAT32 + AHCI together), `make run-part-fat` /
  `run-part-embkfs` (MBR-partitioned disks), `make run-vga-std` /
  `run-virtio-gpu` (display-path variants), `make run-usb-uhci` / `-ohci` /
  `-ehci` / `-xhci` (per-generation USB HC tests).
- **Run — boots to the graphical desktop** (needs `make embkfs.img` first):
  `make run-embkfs-tree` (2-level EMBKFS image with every app), `make run-embkfs-cow`
  (boots a pristine EMBKFS image, then grades the post-boot copy against the
  Python oracle), `make run-embkfs-encrypted` (AES-256-XTS volume, test
  passphrase `correcthorsebattery`), `make run-multivol`, `make run-usb-embkfs`
  (EMBKFS over xHCI mass storage).
- **Run — the UI toolkit specifically:** `make run-ui` (boots to a shell;
  type `run /uidemo.elf` to launch the live EmUI demo), `make run-wm`
  (boots straight to the window-compositor demo, two composited windows,
  ESC quits).
- **Test the UI toolkit without QEMU** (host-compiled, host-run, seconds not
  minutes): `make scene-test backend-test font-test layout-test reactive-test declare-test`.
  `make showcase-v2` (needs Pillow) renders EmUI DSL/V4/V5 screens to
  `build/v2_*.png`/`build/v4_*.png` for a visual check before booting a VM —
  see [`docs/EMUI_GUIDE.md`](docs/EMUI_GUIDE.md#testing-without-booting-a-whole-os).
- **Debug:** `make debug` starts QEMU frozen with a GDB stub on `:1234`; connect
  with `gdb kernel/kernel.elf` → `target remote localhost:1234`.

## The oracle

EMBKFS has a host-side Python formatter/verifier (`embkfs_mkfs/`) that is the
**ground truth** for on-disk correctness. Kernel-side filesystem code is
validated against byte-identical output from the oracle. When you change the
on-disk format or the write path, the oracle is what proves the kernel got it
right — update it alongside.
