# Contributing to EmbLinkOs

EmbLinkOs is a complete x86_64 operating system built from absolute zero — custom
bootloader, kernel, filesystem, userspace, all hand-written with the goal of
understanding every line. This document describes how work is done here so the
project stays coherent and every change is something you could rebuild and
explain.

If you're returning to this after time away, or picking it up fresh, start by
reading **`ARCHITECTURE.md`** (the intended design and the decisions behind it),
then **`PROJECT_STATUS.md`** and **`TODO.md`** (what's actually built vs. what's
left). The repo is ground truth; the docs describe intent.

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
   `ARCHITECTURE.md`, not lost in a commit body.
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

## Docs are part of the change

Documentation drift is the default failure mode. Update, in the same change:

- **`PROJECT_STATUS.md`** — when a phase completes or the build state changes.
- **`ARCHITECTURE.md`** — when a design decision lands, or a planned item
  (🎯) becomes built (✅).
- **`TODO.md`** — add gaps the change leaves behind; remove items it closes.

Naming what a change *doesn't* handle is part of the discipline. A known,
tracked limitation is fine; a silent one is a trap for whoever hits it next.

## Pull requests

PRs use the template in `.github/pull_request_template.md`, which prompts for
exactly these things: what changed, how it was proven, SDM/spec references for
hardware changes, docs synced, and gaps left behind. Fill it honestly — the
"how it was proven" section is the important one.

## Project conventions

- **Target:** x86_64 first, ARM64 later. Single-core today, but written to be
  SMP-safe where it's cheap (avoid hidden global mutable state; prefer passing
  state explicitly over stashing it in a global).
- **Kernel:** higher half at `0xFFFFFFFF80100000`. Boot is a custom two-stage
  BIOS bootloader (UEFI is on `TODO`).
- **Diverge from Unix only with a concrete technical justification** (see
  `ARCHITECTURE.md` §3). Bless the clean native primitive; provide the
  compatible one as an opt-in layer.
- **Formatting:** `%lu`/`%lx` for 64-bit in `kprintf`, `%08X` (cast) for 32-bit.
- **No `-Wall` in build flags** (it fights the packed-struct access the on-disk
  formats need); instead, `memcpy` into aligned locals rather than dereferencing
  packed members directly.

## Build & run

- **Environment:** Ubuntu, `x86_64-elf` cross compiler (`/usr/local/cross/bin`),
  NASM, QEMU, GDB.
- **Build:** `make`
- **Run:** `make run` (base), plus targeted targets — `make run-embkfs-cow`
  (boots a pristine EMBKFS image, then grades it against the Python oracle),
  `make run-ahci`, `make run-fat`, `make run-part-embkfs`, etc.
- **Debug:** `make debug` starts QEMU frozen with a GDB stub on `:1234`; connect
  with `gdb kernel/kernel.elf` → `target remote localhost:1234`.

## The oracle

EMBKFS has a host-side Python formatter/verifier (`embkfs_mkfs/`) that is the
**ground truth** for on-disk correctness. Kernel-side filesystem code is
validated against byte-identical output from the oracle. When you change the
on-disk format or the write path, the oracle is what proves the kernel got it
right — update it alongside.
