# EmbBuild — the native build tool, and why

**Status:** design ratified 2026-07-19. Empirical basis: `test tcc tally`
(kernel/selftests.c:1881) — the make-equivalent hand-unrolled: per-unit
`tcc -c` argvs, one link argv, one install, one oracle. v1 scope: §10.
Format and method per docs/USERSPACE.md: decisions from invariants up,
the tool's on-disk tree derived last.

---

## 1. The deciding constraint, already run

The fork (port make vs. build native) was not settled by philosophy.
The question "what does rebuilding the userland actually require?" was
answered by running it: `test tcc tally` rebuilds a real sval pipeline
tool ON the OS — four compile argvs, one link argv against
`/system/abi`, an install, and an oracle cross-check (`ls / | tally`
vs the builtin `count`). Rebuilding the userland is N copies of that
shape. Every fact below is earned by that run, not guessed.

## 2. Decision — EmbBuild, a typed-manifest walker

**Targets are typed records. Recipes are argv arrays. The tool is a
data-structure walker, not an interpreter.**

1. **Recipes are one `spawn()` each.** The tally rebuild needed
   exactly: sources, an include dir, an object list, link inputs, an
   output path. No recipe in the real graph needs more. No `/bin/sh`,
   no string splitting, no quoting — the failure class of make's
   string world (the same small-integers-in-the-wrong-namespace shape
   as this series' `embk_close_handle(fd)` bug) is structurally absent.
2. **A source-tree convention, discovered not designed:**
   `/data/src/<project>/` with tree shape preserved — forced by
   tally's quote-includes. A manifest names a source root and `-I`
   dirs; that is the whole project model.
3. **The ABI as ambient constants.** `/system/abi/{crt0.o,
   syscalls.o, libc.a}` makes the link line near-constant across all
   targets. No PATH: EmbBuild spawns absolute argvs (USERSPACE.md
   §4.2, applied).
4. **What it deliberately does not have:** variables, functions,
   pattern rules, parallel jobs. Make's power features exist to
   compress graphs too big to hand-write; the real graph is ~50
   one-argv nodes. Start explicit; earn compression when a manifest
   becomes painful to write.

## 3. The sealed boundary — EmbBuild STAGES; adoption is an update

The v2 headline ("the shell rebuilds the shell") produces a new
`shell.elf` — and `/system/bin` is sealed (USERSPACE.md §3). The
resolution is already written there: *"a self-rebuild PRODUCES a new
sealed image; ADOPTING it is an update event."* Therefore:

- **EmbBuild never writes into `/system`. Ever.** All outputs land in
  staging: `/data/build/out/<project>/`.
- **Apps install directly:** for `/data/apps/<name>/` targets, the
  install step is a copy within mutable state — EmbBuild may do it.
- **System programs are ADOPTED, not installed:** crossing the seal is
  a separate, deliberate act — v1: snapshot, copy, reboot, by hand;
  later: a `sysupdate` tool owning that boundary (and, eventually,
  re-signing). A build tool that could overwrite `/system/bin` would
  be a system-update mechanism wearing a build tool's name; keeping
  them separate is the difference between this design and
  `curl | sudo sh`.
- v2's true shape, gained for free: build the shell to staging, VERIFY
  the staged artifact (run it `-c` against the shell-test
  expectations — the oracle pattern), only then adopt.

**✅ DONE (`test embbuild shell`).** The shell rebuilds the shell and the
first adoption event is real: 12 units TCC-built to staging; the STAGED
shell passes the `-c` oracles (expression eval, `where`/`sort-by`/`select`
pipeline, an extern `tally` pipeline); the system snapshots
(`pre-shell-adopt`) and copies staged → `/system/bin/shell.elf` (191,904
bytes); the ADOPTED shell passes the same oracles — every pipeline that
reaches `/system/bin/shell.elf` (the terminal, `test extern`) now runs
through a shell the system built for itself, on its next spawn. The build
tool never touched `/system`: the manifest has no install stanza, and the
seal-crossing copy is the system's act. (This also flushed out
`SPAWN_ARGV_MAX` 16 → 32 — the first machine-generated argv, a 19-entry
link line, outgrew a limit sized for hand-typed commands.)

Two boundaries stated plainly, so the headline is not mistaken for more than
it is:
- **Verification depth.** The staged/adopted shell is proven by three `-c`
  oracles (expression eval, a `where`/`sort-by`/`select` pipeline, an extern
  `tally` pipeline), *not* the full 38-test host suite. It is "a working
  shell across the paths that matter," not "byte-for-byte the host shell"
  (different compiler — nor should it be).
- **Adoption depth.** The ritual exercised is snapshot → copy → re-spawn.
  The `reboot` leg of §3 was not run, and nothing at boot spawns the shell
  anyway (boot goes to `home.elf`); "adopted" here means the file is
  replaced and used on next spawn, with the snapshot as the rollback.

## 4. Staleness — content, not time

The RTC resolves to one second; TCC compiles in milliseconds.
Edit-then-rebuild-within-a-second false-fresh is the COMMON case here,
not the corner — timestamp staleness is disqualified on this machine.

- **v1: stamp files.** Per target, hash of: all input bytes + the full
  argv + the tool identity (compiler path and EmbBuild's own version —
  a flag-only or compiler upgrade must rebuild; make gets this wrong
  too). Stamps live in `/data/build/stamps/`.
- **Hash: CRC32C**, already the house function. Threat model stated
  honestly: ACCIDENTAL collision (~2^-32 per pair) — adversarial
  collision is not in scope for a local build stamp. Upgrade the
  function if that ever changes.
- EMBKFS's per-block CRC32C is internal, not exposed as a file
  identity through `stat` — so v1 hashes bytes in userspace (fine at
  these sizes). "Expose a cheap content-version from the CoW
  generation machinery" is a real kernel item later, pulled by need.

## 5. The manifest — the shell's own value model

Internally, a manifest is sval records: a table of targets
(`name, kind(compile|link|install), inputs, argv, output`). EmbBuild
is built against the sval SDK — typed records, a serializer, and the
code the OS just proved it can rebuild.

Concrete v1 syntax (my recommendation, veto open): a minimal
hand-writable, diff-able text form parsed into sval records —
one record per stanza, `key: value` lines, lists whitespace-split.
Shell-native literal syntax as the manifest surface is a v2 option
once the shell has one worth standardizing. When stdout is a pipe,
EmbBuild emits its plan/results as a typed table (`embbuild | where
state == stale`), fd-3 convention as with every sval tool.

## 6. The honest boundary (named exclusions, each a TCC fact)

"Rebuild-self" with TCC means the STATIC C userland: the shell, the
sval tools, EmbBuild itself. Excluded, with reasons already proven in
the tree: `__thread` (no PT_TLS via tcc link), `libembk.so` apps (the
dynamic path is gcc-shaped), C++, and the kernel (wants GCC). None are
EmbBuild's problem — they are compiler facts, and they do not shrink
the claim: THE SYSTEM CAN REBUILD THE SYSTEM'S OWN PROGRAMS.

## 7. Where make lives

The ports story: rebuilding git or CPython on-OS means autotools, sh,
sed, a POSIX layer — an epoch entered deliberately when a foreign tree
demands it (USERSPACE.md §4.3's seam), never a dependency smuggled in
by the native tool. Fourth instance of the fork, same resolution as
the first three — with the difference that this time the native
option's primitive already ran. Exit 42.

## 8. EmbBuild itself

- **An application:** `/data/apps/embbuild/embbuild.elf`. By D2's own
  logic the orchestrator of compilers is no more sealed than the
  compilers. Consequence: EmbBuild rebuilds EmbBuild without touching
  the seal.
- **Host-bootstrapped once** (like TCC), then self-hosting: its own
  manifest is target #3 after tally and sysinfo.

## 9. The tree — derived

```
/data/build/
├── out/<project>/        staged artifacts (NEVER /system; §3)
└── stamps/<project>/     content stamps (§4)
```
`clean` is honest and total: `rm -r /data/build`. All tool-owned state
lives in one deletable directory.

## 10. v1 scope + acceptance — ✅ COMPLETE, including target #3

All three targets are live (`test embbuild` a–f + `test embbuild self`):
tally and sysinfo rebuild from `/data/src` via manifests, and **EmbBuild
rebuilds EmbBuild** — the TCC-built successor is staged, installed to
`/data/apps/embbuild/` (an apps write, no seal crossed), and cross-checked
two ways: the STAGED successor reruns the tally manifest and reports
`0 ran, 6 up_to_date` (a gcc-built tool and its TCC-built child agreeing on
the state of the world — the two-implementations oracle), and the INSTALLED
successor reruns its own manifest to the same verdict. Closing the loop
surfaced two real bugs beneath it: the ABI's syscall stubs were gcc-only
(`register …asm("r10")` bindings tcc ignores — embk_syscall.h now carries a
`__TINYC__` branch passing high args through memory), and **tcc 0.9.27 never
relocates the GOT in static links** (the relocation walk skips `s1->got`;
right with a dynamic loader, NULL-deref without one — every newlib
`stderr`/`errno` is a GOTPCREL `_impure_ptr` access). That is
`tools/tcc/0003-static-link-relocate-got.patch`, sibling of 0001.

Original acceptance definition, all exercised: green =
(a) both oracles pass on the staged binaries; (b) no-change rebuild is
a no-op (stamps hit); (c) one edited byte rebuilds exactly that unit's
chain; (d) one changed flag rebuilds despite identical sources —
the false-fresh case make fails, exercised as a selftest per the
house rule: a change is not done until a test exercises the invariant.

## 11. What would reopen this

A manifest that becomes genuinely painful to hand-write (earns
compression features, §2.4); a foreign source tree on the critical
path (earns the make port, §7); staging-vs-adoption friction so
constant it argues the boundary is drawn wrong (§3).
