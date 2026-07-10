# EmbLinkOS — Process & Scheduling Architecture

*Subsystem specification. Companion to `docs/ARCHITECTURE.md` §2 (Stack), §3.2–3.3 (settled decisions this spec builds on), §5 (roadmap item 3). Written against the kernel as of 2026-07-05 (commit range through the process-init bring-up on `Teo`).*

*Update, same day: Phase A of the roadmap (§13) landed — zombie reclamation and the kernel-stack guard page (Bugs 3 and 4 below) are now fixed, and a fifth bug (PML4-sharing) was found and fixed while verifying the guard-page fix end to end. Sections below are updated in place; nothing here was re-derived, only corrected against what actually shipped.*

*Update, later the same day: Phase B landed in full — wait queues, timer-driven preemption, the uncatchable kill, per-process fd tables, and `sys_wait`/`sys_spawn`/`sys_yield`/`sys_getpid`. Four more bugs were found and fixed while verifying Phase B end to end (Bugs 6–9, §16), all specific to the interaction between real preemption and code that had only ever run with a single process. Sections below are again updated in place against what actually shipped, not re-derived.*

*Update, 2026-07-06: Phase 4 (the real `struct thread`/`struct process` split, §4.1's long-deferred half) landed — `process.c` rewritten in full around `thread_table[MAX_THREADS]` (the schedulable unit) + `process_table[MAX_PROCESSES]` (the resource owner: address space, pid, parent/child/zombie tracking, fd/handle tables), with `current_thread` (a real per-CPU field, `cpu_table[]`) and `current_process` (a derived, read-only `current_thread->proc` macro) replacing the old single `current_process` field everywhere. Two new selftests (`test thread smp`/`test thread exit`) plus the full existing 8-test regression battery all pass, both under `-smp 4`, across two independent clean boot-to-battery runs — zero new bugs found this phase, the first phase in this subsystem's history where that's true (see §16's closing note). Sections below are updated in place.*

*Update, 2026-07-09: Phase 5 (ring-3 threads) landed — `thread_create_user()`/`thread_join()`/`thread_exit_self()` (process.c) plus three new syscalls (`sys_thread_create`/`sys_thread_join`/`sys_thread_exit`) give a ring-3 process the same real multi-threading Phase 4 gave the kernel, with each extra thread getting its own dedicated, deterministically-placed user stack inside the SAME process address space. `MAX_PROCESSES`/`MAX_THREADS` raised 16/16 → 64/256 now that a single process can genuinely want more than one thread (with `KSTACK_SIZE`'s accidental coupling to `MAX_PROCESSES` caught and fixed in the same pass — see §6.1). `user/init.c` now spawns and joins a second thread of itself as part of its own startup, proving real shared-memory threading end to end via a new `test ring3 threads` selftest. Verification this phase also surfaced (and mitigated, not fully eliminated) a real test-robustness finding in the pre-existing `test thread smp` — a correlated/bursty timing artifact of its original fixed sample size, not a scheduler bug — see §12/§13's Phase 5 notes. Sections below are updated in place.*

**Status legend:** ✅ Built · 🚧 Built but wrong/incomplete (see Common Bugs) · 🎯 Specified here, not yet built · ⏳ Deferred

---

## 1. Purpose

The process and scheduling subsystem is the kernel's answer to one question: **when the CPU is free, what runs next, and how does it get there safely?** Everything downstream — the syscall ABI, IPC, the eventual `spawn()` primitive, SMP, even the shell — is built on top of the guarantees this subsystem makes. Get the primitives wrong here and every layer above inherits the wrongness; this is why it's the first subsystem to receive a full spec.

Concretely, this subsystem owns:

- The **process control block (PCB)** — what a process *is*, in kernel memory.
- **Context switching** — the mechanical act of suspending one instruction stream and resuming another.
- **Scheduling policy** — the algorithm that picks the next process to run.
- **Process lifecycle** — creation, the ring 0 → ring 3 transition, blocking, termination, and reclamation.
- **The uncatchable kill** — the kernel's ability to end a process regardless of what it is doing (settled in `docs/ARCHITECTURE.md` §3.3; ships with this subsystem, not later).

It explicitly does **not** own: the ELF loader (`cpu/elf.c`), address-space construction (`mm/vmm.c`), or the syscall dispatch table (`cpu/syscall.c`) — those are dependencies, described in §14, not re-specified here.

---

## 2. Goals

1. **Correctness before cleverness.** A scheduler bug corrupts every process on the system, silently, hours later. Every invariant in §7 must be checkable, and violated invariants must fail loudly (panic), never limp on.
2. **A clean seam for policy change.** Round-robin today, priority/MLFQ tomorrow, SMP-aware per-CPU queues after that — the *mechanism* (context switch, PCB, state machine) must not need to change when the *policy* (which PCB runs next) does. This is the same ops-vector discipline `docs/ARCHITECTURE.md` §3.1 already mandates project-wide.
3. **The uncatchable kill is not optional.** `docs/ARCHITECTURE.md` §3.3 chose a cooperative message-port model for ordinary IPC specifically *because* Unix signals are a historical accident (arbitrary-instruction-boundary handlers, `EINTR`, thread-hostility — expanded in §4 below). But a purely cooperative model has one hole: a process that never polls its message queue can't be stopped by a message. The kernel must be able to terminate any PID unconditionally. This is a scheduler-level guarantee, not a userspace convention.
4. **`spawn()`-shaped from day one.** Per `docs/ARCHITECTURE.md` §3.2, process creation is `spawn()` (build the address space directly, apply file-actions), not `fork()`+`exec()`. The PCB and creation path should not carry `fork()` assumptions (e.g., "duplicate everything then diverge") that would have to be undone later.
5. **SMP-ready shape, not SMP-complete.** Per `docs/ARCHITECTURE.md` §7, SMP itself is deferred. But the data structures chosen now (run queues, locks) should be the kind that generalize to per-CPU instances, not the kind that require a rewrite (see §8 Synchronization).

---

## 3. Current State (ground truth, not aspiration)

This section exists because `docs/ARCHITECTURE.md` §2 lists the scheduler as "🎯 Designed" — accurate for the *policy* decisions (§3.2, §3.3) but the actual code is far more primitive than that status implies. Recording the real state prevents this document from being read as describing something already built.

| Property | Actual state |
|---|---|
| Process/thread tables | ✅ Split (Phase 4). Static arrays: `thread_table[MAX_THREADS = 256]` (the schedulable unit — ctx, kstack, state, priority, running/pinned CPU) and `process_table[MAX_PROCESSES = 64]` (the resource owner — pid, pml4, parent/child/zombie tracking, fd/handle tables). Raised from 16/16 in Phase 5 once a ring-3 process could genuinely want more than one thread (see §6.1) — `KSTACK_SIZE`'s accidental coupling to `MAX_PROCESSES` was caught and decoupled in the same pass, or raising the ceiling would have silently quadrupled every kernel stack's size. Every process created by `process_create()`/`process_create_kthread()` still gets exactly one thread by default; `thread_create()`/`thread_create_user()` are the primitives that let a process have more than one, sharing its address space. |
| Scheduling policy | ✅ Built (Phase C). Fixed priority bands (`SCHED_PRIORITY_BANDS = 4`: REALTIME/INTERACTIVE/NORMAL/BACKGROUND), round-robin within a band, with aging (`PRIORITY_AGE_TICKS = 20`, ~200ms/band) bumping a starved READY process up one band so a busy high band can't starve a low one forever. Verified via `test sched priority`. |
| Preemption | ✅ Built. `lapic_timer_handler` calls `schedule()` on every tick (100Hz) after `lapic_send_eoi()` — genuine timer-driven round-robin, not just syscall-triggered. See Bugs 6 and 7, §16, both found while verifying this. |
| Blocking | ✅ Built. `struct wait_queue` + intrusive `process::wait_next`; `wait_queue_block`/`wait_queue_wake_one`/`wait_queue_wake_all` implement §7.3 as specified. Now genuinely exercised: `process_wait()`'s blocking path (below) and `sys_wait` both use it for real, not just the selftests. |
| Termination reclamation | ✅ Fixed and now exercised. `process_reap()` frees the PCB/kernel stack/address space, deferred one `schedule()` call behind the actual exit (see §7.4). Previously unexercised in practice; now proven live by `test sched reap` and by real multi-process use (`sys_spawn`/`sys_wait`). See Bug 3, §16. |
| Uncatchable kill | ✅ Built. `process_kill(pid)` forces `PROCESS_ZOMBIE` regardless of current state (including `BLOCKED`, unlinking from whatever wait queue it's on), reaps immediately unless it's the caller itself. Verified via `test sched kill`. |
| Parent/child tracking + real blocking wait | ✅ Built (closes what was Phase D). `struct process` has `parent`/`parent_pid` (the latter guards against a recycled PCB slot aliasing an unrelated new process, see `parent_is_alive()`), `child_list`/`child_next` (live children, for `ps`), and `zombie_head`/`zombie_next` (a parent's exited-but-unclaimed children — deliberately two separate fields, not one double-duty field, see §16's discussion of why). `process_wait(pid)` genuinely blocks on `child_wait` until woken by the specific child's exit or kill, rather than busy-polling. See Bug 11, §16 for a real deadlock found and fixed while building this. |
| Ring-3 process handles | ✅ Built. Per-process `struct proc_handle handles[PROC_HANDLE_MAX]` translates a small ring-3-visible integer to a real pid (`process_handle_alloc/resolve/free`); `sys_spawn` returns a handle, `sys_wait`/`sys_kill` take one. Closes the confused-deputy gap a raw pid argument left open (any ring-3 process could otherwise name any pid it could guess). |
| Per-process file descriptors | ✅ Built. `struct process` now embeds `struct fd_entry fds[FD_MAX_OPEN]`; `fs/fd.c`'s `fd_table()` helper returns it (or a boot-time-only global table before any process exists). Unblocks `spawn()`'s file-action model (§3.2). |
| Kernel stack guard page | ✅ Fixed. `vmm_alloc_kernel_stack`/`vmm_free_kernel_stack` page-map the stack with an unmapped guard page directly below it, replacing the flat `kmalloc`. See Bug 4, §16 — and Bug 5, found while verifying this fix. |
| Syscalls reaching this subsystem | ✅ Built. `sys_exit`, `sys_yield`, `sys_spawn`, `sys_wait`, `sys_getpid`, `sys_kill` all wired to syscall numbers and dispatched from `syscall_dispatch` — the full §15.2 surface. |
| Interactive process control | ✅ Built. The kernel's own shell (`main.c`) is no longer a one-way `process_start_first()` hand-off — it calls `process_adopt_current()` to become a real, permanent, round-robin-scheduled `current_process` itself, then `run`/`ps`/`kill`/`wait`/`nice` shell commands call straight into this subsystem's kernel-internal API (no handle indirection needed there — trusted code, not a ring-3 caller). Verified interactively in QEMU via monitor-injected keystrokes against a real `/init.elf`. |
| Automated selftests | ✅ Built. `test sched roundrobin`/`kill`/`reap`/`stackguard`/`wait`/`priority`, `test smp sched`/`kill`, `test thread smp`/`exit` (`process_test_*` in `process.c`) — ten selftests total, the original four plus two added alongside `process_wait()`/priority bands, two added for SMP, and two added for the Phase 4 thread/process split. |
| SMP | ✅ Built (see the Phase SMP roadmap entry, §13). `current_thread` is a real per-CPU field (`cpu_table[]`, `kernel/cpu/percpu.h`); `current_process` is derived from it (`current_thread->proc`). |
| Thread/process split | ✅ Built (Phase 4, §4.1's long-deferred half). `struct thread` (schedulable unit) and `struct process` (resource owner) are now genuinely separate structs — see §6.1/§6.2. `thread_create(proc, entry)` lets one process have more than one kernel thread, verified by `test thread smp` (3 threads, 1 process, shared address space, ≥2 distinct cores) and `test thread exit` (address space survives until the LAST thread exits, not the first). |
| Ring-3 threads | ✅ Built (Phase 5, closes what Phase 4 deliberately deferred). `thread_create_user()`/`thread_join()`/`thread_exit_self()` (process.c) plus `sys_thread_create`/`sys_thread_join`/`sys_thread_exit` (§15.2) give a ring-3 process the same real multi-threading Phase 4 gave kthreads — an additional thread sharing the SAME process address space, entering ring 3 directly, with its own dedicated user stack (§6.1). Joinable, not auto-reaped, unlike a kthread: a thread created this way sits as a zombie until `thread_join()` collects its exit code, mirroring how a process zombie waits for `process_wait()`. Verified end to end by `test ring3 threads`, which spawns `/init.elf` (via the REAL scheduler, `process_create()`/`process_wait()` — not the standalone `enter_user_mode()` path `test ring3` uses) and checks an exit code that only comes out right if the child's own `sys_thread_create`/`sys_thread_join` and a genuinely shared `.data` write all worked. |

Twenty-five concrete bugs were found (and all twenty-five fixed) getting this subsystem from Phase A through the SMP phase, documented in full in §16, Common Bugs, because they are the kind of bug this class of code produces repeatedly — worth remembering the *shape* of the mistake, not just the fix. The first five were found bringing up a single process (Phase A); the next four were found bringing up real preemption, multi-process syscalls, and per-process fd tables (Phase B) — every one of them a case of code that was correct for exactly one process silently breaking the instant a second process and real preemption coexisted. Bug 10 is a different provenance worth naming explicitly: it was caught by *re-reading* the finished Phase B work while writing this document, not by an observed crash or a failing test — a reminder that "no test caught it yet" and "it isn't there" are not the same claim. Bug 11 is back to the usual provenance (a genuinely new, previously-unexercised code path — real blocking `process_wait()` — immediately hanging its own first selftest run). Bugs 12–25 were the SMP bring-up ledger (§16). **Phase 4 (the thread/process split) added zero new bugs** — the first phase in this subsystem's history of which that's true, credited to the split being designed explicitly around the two hazards SMP had already taught (idempotent re-entry into exit-disposition logic, and a belt-and-suspenders `running_cpu` liveness check) rather than being new territory:

1. **The ring-3 entry trampoline pushed the wrong CS selector** (a literal immediate instead of the intended register operand) — corrupted `iretq` frame, `#GP` on every process start.
2. **`kstack_top` was declared `uint16_t`** in a struct that stores a 64-bit heap address in it — silent truncation that would have corrupted `TSS.RSP0` the first time a ring-3 process took any interrupt.
3. **Zombie processes were never reclaimed** — closed a hard ceiling of exactly `MAX_PROCESSES` process creations ever.
4. **No kernel-stack guard page** — a stack overflow silently corrupted adjacent heap memory instead of faulting.
5. **Fixing #4 introduced a new bug**: the replacement kernel-stack VA region landed in a PML4 slot never touched before boot, invisible in the first process's own page tables (`vmm_create_address_space()` shares the kernel half by copying PML4 entries *by value* at creation time, not live) — `#PF` cascading to `#DF` on the very first process. Caught immediately by actually running `/init.elf` end to end rather than trusting the build + a no-filesystem boot smoke test.
6. **RFLAGS.IF corruption across a context switch** — `kernel_ctx_switch`/`kernel_ctx_save` capture RFLAGS with `pushfq` from inside an interrupt-gate ISR, where IF is always 0 by construction, so every saved snapshot claimed IF=0 regardless of the outgoing process's real state; the first resumed process therefore came back with interrupts permanently masked.
7. **`int 0x80` leaves IF=0 for the entire syscall** — same underlying fact as #6 (interrupt gates auto-clear IF), different symptom: any syscall blocking on a hardware completion IRQ (disk I/O inside `sys_open`) hung forever because the satisfying IRQ could never fire.
8. **`proc_alloc()` marked a brand-new PCB `PROCESS_READY` before it was initialized** — harmless with no preemption, but the instant real preemption (#6/#7 above) and a second `process_create()` call could interleave, the scheduler could pick a half-built PCB mid-ELF-load and crash.
9. **PCB leak on every `process_create()` error path** — none of the four early-return failure paths reset `state` back to `PROCESS_UNUSED`, permanently leaking the slot on any failed creation.
10. **`schedule()` itself was reentrant against the timer ISR when called from syscall context** — `syscall_dispatch()`'s `sti` (needed for Bug 7) meant `schedule()` calls from `sys_exit`/`sys_yield` ran with IF=1, so a timer IRQ could land mid-`schedule()` and re-enter it while the outer call was still mutating shared scheduler state.
11. **`schedule()`'s zombie hand-off ran after the "nothing else to switch to" early return, deadlocking `process_wait()`** — a dying process's parent-hand-off/auto-reap decision was reachable only once a *different* runnable process had already been found, but the one scenario `process_wait()` exists to create is exactly the parent sitting `BLOCKED` (not READY/RUNNING) with nothing else runnable — so the hand-off that would wake it never ran, and neither process was ever scheduled again. Found immediately by `test sched wait` hanging the very first time it ran.

---

## 4. Comparative Analysis — How Other Systems Do This

This section exists because the governing principle in `docs/ARCHITECTURE.md` §1 is *"diverge from Unix only with a concrete technical justification."* You cannot apply that principle without first knowing what you're diverging from and why it was built that way.

### 4.1 The process/thread object model

| System | Model |
|---|---|
| **Linux** | `task_struct` represents both processes and threads uniformly (a thread is a `task_struct` that shares an `mm_struct` with its "process"). `fork()` is `clone()` with all sharing flags set; threads are `clone()` with `CLONE_VM\|CLONE_FS\|CLONE_FILES\|...`. Elegant unification, but it means "process" isn't a first-class kernel concept — it's a *convention* over the thread primitive, which occasionally leaks (thread-group leader semantics, `getpid()` vs `gettid()`). |
| **Windows (NT)** | Explicit two-level object model: `EPROCESS` (address space, handle table, security token) contains one or more `ETHREAD` (execution context, scheduling state). Threads are genuinely separate kernel objects with their own scheduling priority, not a sharing-flag variant of a process. Closer to how programmers actually think about the two concepts. |
| **XNU (Apple)** | Splits it three ways: BSD `proc` (POSIX-facing: pid, signals, fds), Mach `task` (address space + IPC rights — the *actual* resource container), Mach `thread` (schedulable unit, owns no resources itself). The Mach layer is what schedules; the BSD layer is a compatibility skin bolted on top for POSIX semantics. This is the most "layered by concern" of the four, and the most complex to reason about. |
| **FreeBSD** | Similar split to Linux conceptually (`struct proc` containing `struct thread` list) but without Linux's `clone()`-flag unification — process and thread are syntactically distinct structs from the start. |

**What EmbLinkOS should do:** keep **process and thread as distinct structs from day one**, following the Windows/FreeBSD shape rather than Linux's clone-flag unification. Reasoning:

- `docs/ARCHITECTURE.md` §3.2 already commits to `spawn()`, which builds a *complete* new address space + first thread in one step — there is no `fork()`-shaped "thread is a process with sharing flags" pressure pushing toward Linux's unification. That pressure is precisely *why* Linux unified them (so `clone()` could parametrize the flags); EmbLinkOS doesn't have `clone()` and shouldn't back into needing it.
- Separating them now costs nothing (a process with exactly one thread is the common case anyway) and avoids the exact retrofit Linux users complain about (`gettid()` existing because `getpid()` was already load-bearing for the wrong thing).
- It matches the ops-vector/handle discipline already chosen in §3.4/3.5: a thread handle and a process handle are different capability types, which only makes sense if they're different objects.

**Current code reality check:** ✅ done (Phase 4, §13). `struct thread` (the schedulable unit — `ctx`, `kstack_top`, `state`, `priority`, `running_cpu`/`pinned_cpu`) and `struct process` (the resource owner — `pid`, `pml4_phys`, parent/child/zombie tracking, `fds`/`handles`) are now genuinely separate structs, exactly the Windows/FreeBSD shape this section argued for. `schedule()`'s scan/dispatch shape (priority bands, round-robin, aging) is textually almost unchanged — it now scans `thread_table[]`, reading `t->proc->pml4_phys` at dispatch. `thread_create(proc, entry)` is the new primitive letting one process own more than one thread; every process still starts with exactly one (`process_create()`/`process_create_kthread()`), so "a process with one thread is the common case" (this section's original reasoning for deferring the split) remains true even now that the split exists.

### 4.2 Scheduling algorithm

| System | Algorithm |
|---|---|
| **Linux (CFS, pre-6.6)** | Completely Fair Scheduler: red-black tree keyed on `vruntime` (virtual runtime, weighted by `nice`); always run the leftmost (least-served) node. No fixed time slices — the slice is derived from the number of runnable tasks and a target latency. |
| **Linux (EEVDF, 6.6+)** | Earliest Eligible Virtual Deadline First — CFS's successor. Tracks a virtual deadline per task and picks the earliest *eligible* one, which handles latency-sensitive tasks more directly than CFS's vruntime-only comparison. |
| **Windows** | Multilevel feedback priority queue, 32 priority levels (0–31; 0 reserved for the zero page thread). Round-robin within a priority level, with **dynamic priority boosting**: a thread that just received UI input, woke from a voluntary wait, or was starved gets a temporary priority bump. This is explicitly optimized for desktop interactivity over throughput fairness. |
| **FreeBSD (ULE)** | Multi-queue (per-CPU) feedback scheduler, interactivity score computed from sleep/run time ratio, separate queues for "current" vs "next" batch to bound worst-case latency. |
| **XNU** | Priority-band scheduler (0–127), with the Mach layer doing timeshare decay for normal bands and strict priority for realtime bands. Multiple runqueues, one family of algorithms per band. |

**What EmbLinkOS should do — phased, not a single final answer:**

1. **Phase A/B (now → blocking):** plain round-robin, exactly what's in the tree today, kept deliberately dumb. No system with fewer than a handful of runnable processes benefits from a fancy scheduler, and building CFS's rbtree machinery before there's even a working `wait()`/blocking model is solving a problem that doesn't exist yet on a system with no preemption.
2. **Phase C (priority):** a small, fixed number of priority *levels* (think Windows' banding, not Linux's continuous `vruntime`) with round-robin within a level, and starvation prevented by periodic priority aging (a READY process not scheduled for N ticks gets bumped a level). This is chosen over CFS-style fairness accounting because **fairness accounting is solving a multi-user timesharing problem EmbLinkOS does not have** — it's a single-developer workstation OS, not a shared server. Windows' "boost the thread that just handled a keypress" instinct is actually the *more* relevant lineage for a desktop-dev-environment OS than Linux's fairness-under-contention design point. This is a case of "study Linux, don't copy Linux" per the philosophy in the top-level system context.
3. **Not planned, ever, unless a concrete need appears:** full CFS/EEVDF-style vruntime/deadline accounting. It solves fairness-under-heavy-multi-tenant-load, which is not this OS's problem. If it ever becomes a problem (this OS ends up running many CPU-bound tenants), revisit — but don't pre-build it.

### 4.3 Interrupts vs. cooperative delivery (context, not a re-decision)

`docs/ARCHITECTURE.md` §3.3 has already settled this (message ports + uncatchable kill, signals as a later compat shim) — restated briefly here only because it constrains the scheduler's blocking design in §7: a blocked process is *waiting on a message port becoming readable*, not "waiting for a signal." The wait-queue primitive in §7.3 is built around that, not around Unix's `sigwait`/`sigsuspend` shape.

### 4.4 Context switch mechanism: software vs. hardware task switching

x86 has a **hardware task-switch mechanism** (the `TSS` descriptor's busy bit, `JMP`/`CALL` to a TSS selector triggering a full hardware-assisted register-state swap). Every production OS — Linux, Windows, xnu, BSD — **does not use it**, and uses a single TSS per CPU purely as a vehicle for `RSP0` (and `IST` stack pointers for exceptions), with context switching done in software (push/pop + a manual stack-pointer swap), exactly what `kernel_ctx_switch` in `cpu/kcontext.asm` already does.

**Why hardware task-switching lost:** it saves/restores the *entire* register file unconditionally (segment registers included) even when only a handful of registers are actually live across the switch, and it does so through a slow microcoded path that pre-dates the introduction of fast software alternatives; it also does not exist at all in **long mode** in a usable form (x86-64 removed hardware task-switching from ring 3 and severely restricted it — Intel SDM Vol. 3A §7.2.1 notes hardware task-switching is not supported in 64-bit mode the way it was in protected mode). EmbLinkOS's existing choice (software switch, one TSS per CPU for `RSP0`/`IST` only) is therefore not really a choice at all in long mode — it's the only option — but it's worth stating explicitly *why*, because it explains the shape of `struct kcontext` (callee-saved GPRs + RSP + RIP + RFLAGS, not "every register") and why `TSS.RSP0` is set **separately**, in software, right before the switch (see §7.2 for the exact ordering and why it matters).

---

## 5. Architecture

```
                     ┌─────────────────────────────────────────┐
                     │              Syscall Layer               │
                     │  sys_exit · sys_spawn(✅) · sys_wait(✅)  │
                     │  sys_yield(✅) · sys_getpid(✅)            │
                     └───────────────────┬───────────────────────┘
                                         │ calls into
                     ┌───────────────────▼───────────────────────┐
                     │         Process Lifecycle Manager          │
                     │  process_create · process_exit_self(✅)    │
                     │  process_reap(✅) · process_kill(✅)       │
                     └───────────────────┬───────────────────────┘
                                         │ mutates PCB, enqueues/dequeues
              ┌──────────────────────────▼──────────────────────────┐
              │                  Scheduler Core                      │
              │  schedule() — policy-pluggable via an ops vector      │
              │  ┌────────────┐  ┌────────────┐  ┌─────────────────┐ │
              │  │ Run queue  │  │ Wait queues │  │ Priority/aging  │ │
              │  │ (Phase A:  │  │ (Phase B,   │  │ (Phase C, ✅)   │ │
              │  │ ring, ✅)  │  │ ✅)         │  │                 │ │
              │  └────────────┘  └────────────┘  └─────────────────┘ │
              │  timer-driven preemption: LAPIC tick → schedule() ✅  │
              └──────────────────────────┬──────────────────────────┘
                                         │ picks next PCB, then:
                     ┌───────────────────▼───────────────────────┐
                     │       Address-Space + TSS Handoff           │
                     │  vmm_switch_address_space(next->pml4_phys)  │
                     │  tss_set_rsp0(next->kstack_top)             │
                     │  — MUST happen before the context switch —  │
                     └───────────────────┬───────────────────────┘
                                         │
                     ┌───────────────────▼───────────────────────┐
                     │        Context Switch Primitive             │
                     │  kernel_ctx_switch (cpu/kcontext.asm)       │
                     │  kernel_ctx_restore (first-run / one-way)   │
                     └───────────────────┬───────────────────────┘
                                         │ resumes at saved RIP
                     ┌───────────────────▼───────────────────────┐
                     │      Ring-3 Entry Trampoline                 │
                     │  process_trampoline — fabricated first RIP  │
                     │  builds the iretq frame, drops to ring 3     │
                     └───────────────────────────────────────────┘
```

**Layering rule:** each box only calls downward or into its own box. The Scheduler Core never touches `vmm_*`/`tss_*` directly inline with policy logic — it calls a single `dispatch(next)` function that does the handoff, so a future SMP-aware scheduler can change *what* `dispatch` does (e.g., IPI another CPU instead of switching locally) without the run-queue/priority logic knowing or caring. This is the ops-vector discipline from `docs/ARCHITECTURE.md` §3.1 applied here specifically.

---

## 6. Data Structures

### 6.1 Process Control Block — current (`process.h`, ✅ built, annotated)

**Superseded by the Phase 4 split (§4.1, §13) — kept below as the historical single-struct shape, with the current split immediately after.** Through Phase SMP, `struct process` conflated the schedulable unit and the resource owner:

```c
struct process {
    uint32_t pid;                  // process ID, monotonic allocator (§7.1)
    uint64_t pml4_phys;            // this process's address space root
    struct kcontext ctx;           // saved callee-saved regs + RSP + RIP + RFLAGS
    uint64_t kstack_top;           // VA of this process's kernel stack top
                                    //   (was uint16_t — Bug 2, §16; fixed)
    uint64_t entry_point;          // ring-3 entry VA, consumed by the trampoline
    uint64_t user_rsp;             // initial ring-3 RSP
    enum process_state state;      // see §7 state machine
    int exit_code;                 // valid once state == PROCESS_ZOMBIE

    /* Phase B, ✅ built: blocking */
    struct process *wait_next;     // intrusive singly-linked wait-queue membership
    struct wait_queue *wait_queue;  // which queue this PCB is currently linked into
                                    //   (NULL if not blocked); lets process_kill (§9)
                                    //   unlink a blocked target without the caller
                                    //   having to know which queue to search

    /* Phase B, ✅ built: per-process resources (unblocks spawn() file-actions, §3.2) */
    struct fd_entry fds[FD_MAX_OPEN]; // was: implicit global table (fs/fd.c's g_fds);
                                       //   embedded by value, not a pointer — see §16
                                       //   discussion, no separate allocation/free path
};
```

**Phase 4, ✅ built — the actual current shape.** The fields above split across two structs: everything `schedule()` touches every tick moved to `struct thread`; everything a process *owns* (and shares across every thread it has) stayed on `struct process`:

```c
struct thread {
    struct kcontext ctx;           // unchanged from above — still opaque outside
                                    //   kernel_ctx_switch/kernel_ctx_restore
    uint64_t kstack_top;           // this THREAD's own kernel stack (was the process's)
    uint64_t entry_point;          // ring-3 entry VA, OR (kthreads) the real function
                                    //   pointer kthread_trampoline stashes here
    uint64_t user_rsp;             // initial ring-3 RSP (ring-3 threads only)
    enum process_state state;      // moved from struct process -- see §7

    struct thread *wait_next;      // wait-queue membership -- now thread-level,
    struct wait_queue *wait_queue;  //   since threads (not processes) block/wake

    int running_cpu;               // which cpu_table[] index this thread is
                                    //   RUNNING on, -1 otherwise (moved from
                                    //   struct process; see process_kill()'s
                                    //   three-way branch, §9)
    int pinned_cpu;                // -1 = any core; >=0 = ONLY that core may
                                    //   dispatch this thread (per-core idle/
                                    //   adopted-shell liveness pin, Phase SMP)

    uint8_t  priority;              // moved from struct process
    uint32_t ticks_since_scheduled; // moved from struct process

    struct process *proc;          // owning process -- never NULL
    struct thread *proc_thread_next; // intrusive link in proc->thread_list
};

struct process {
    uint32_t pid;
    uint64_t pml4_phys;            // shared by every thread of this process

    struct process *parent;
    uint32_t        parent_pid;
    struct process *child_list;
    struct process *child_next;
    struct process *zombie_head;
    struct process *zombie_next;
    struct wait_queue child_wait;

    int exit_code;                 // valid once every thread has exited
    int live_thread_count;         // process is reapable only once this hits 0
    struct thread *thread_list;    // this process's threads (proc_thread_next-linked)
    int running_cpu;               // meaningful ONLY once live_thread_count == 0:
                                    //   mirrors the completing thread's own
                                    //   running_cpu at that instant -- see the
                                    //   field's comment (process.h) and §7.4

    struct fd_entry fds[FD_MAX_OPEN];
    struct proc_handle handles[PROC_HANDLE_MAX];
};
```

What moved and why, field by field:

- `ctx`/`kstack_top`/`entry_point`/`user_rsp`/`state` all moved to `struct thread` unchanged in meaning — they were always properties of *one execution context*, and the single-struct shape only got away with calling that "the process" because there was never more than one.
- `wait_next`/`wait_queue` moved to `struct thread` because it's genuinely a THREAD that blocks and wakes — a process with multiple threads can have some blocked and some running simultaneously, which the old shape had no way to express.
- `running_cpu`/`pinned_cpu`/`priority`/`ticks_since_scheduled` moved to `struct thread` for the same reason: they're per-schedulable-unit facts, and `schedule_locked()`'s scan now iterates `thread_table[]`, not `process_table[]`.
- `pml4_phys`/`parent`/`parent_pid`/`child_list`/`child_next`/`zombie_head`/`zombie_next`/`child_wait`/`fds`/`handles` all stayed on `struct process` — every one of them is a resource or a piece of family-tree bookkeeping that every thread of a process shares identically, and splitting them per-thread would be actively wrong (two threads of one process must see the same address space and the same fd table, by definition).
- `exit_code` stayed on `struct process` — it's meaningful once, for the whole process, when the LAST thread exits (`live_thread_count == 0`), not once per thread.
- `live_thread_count`/`thread_list` are new: the process becomes a candidate for the parent-hand-off/auto-reap decision only once `live_thread_count` reaches 0 — the direct generalization of "the address space is torn down only once nothing can execute under it" from 1 thread to N. See `thread_zombie_locked()` (process.c) and §7.4.
- `running_cpu` on `struct process` is new and easy to miss the reason for: the parent-hand-off (§7.4) is posted *before* the dying thread's core has confirmed it switched away, so a parent's `process_wait()` can briefly observe a zombie process whose last thread is technically still executing on another core. This field mirrors that thread's own `running_cpu` at the exact moment `live_thread_count` hits 0, giving `process_wait()`'s belt-and-suspenders retry (§16, Bug 23's fix) a place to live now that `running_cpu` itself moved to `struct thread`.

`current_thread` (a real per-CPU field, `cpu_table[]`) and `current_process` (a derived, read-only `current_thread->proc` macro) replace the old single `current_process` field — see process.h's comment on the macro pair for the full assignability/NULL-safety rules. Every external consumer outside `process.c` (`cpu/syscall.c`, `cpu/usercopy.c`, `fs/fd.c`) needed either zero changes (reads `current_process->field` only after already knowing a process exists) or a one-line NULL-check fix (`current_process` → `current_thread`, since `current_thread->proc` can't be evaluated when `current_thread` itself is NULL) — the split was designed from the start to be invisible outside this file, and it shipped that way.

**Phase 5, ✅ built — ring-3 threads add three more fields to `struct thread` and one to `struct process`:**

```c
struct thread {
    /* ... Phase 4 fields above, unchanged ... */

    bool joinable;      // true only for a thread from thread_create_user() --
                         //   see below for why this changes reap timing
    int  exit_code;      // THIS thread's own exit code (thread_exit_self()'s
                         //   arg) -- distinct from struct process::exit_code
    uint64_t user_arg;   // loaded into RDI just before this thread's very
                         //   first ring-3 instruction (process_trampoline)
};

struct process {
    /* ... Phase 4 fields above, unchanged ... */

    struct wait_queue join_wait;   // woken whenever ANY thread of this
                                    //   process exits; thread_join()'s
                                    //   callers block here (see §7.3)
};
```

- `joinable` is the load-bearing addition: a kthread (`thread_create()`) or a process's own main thread stays `false` and keeps the pre-Phase-5 behavior exactly (auto-reaped the tick after it exits, via `pending_thread_reap` — §7.4). A `thread_create_user()` thread is `true`, which makes `schedule_locked()`'s deferred-reap step skip it — it sits as a `PROCESS_ZOMBIE` (kernel stack intact) until `thread_join()` explicitly collects `exit_code` and reaps it, the exact same "sits as a zombie until collected" shape a process already has via `zombie_head`/`process_wait()`, just one level down (a thread within a process, instead of a process within its parent).
- `exit_code` (thread-level) exists because `struct process::exit_code` is process-wide and "last writer wins" across every thread that calls `thread_exit_self()` (see that function's comment, §15.1) — a sibling calling `thread_join(tid)` needs THIS specific thread's own code, not whatever the process-wide field happens to hold at that moment.
- `user_arg` is the ring-3 thread's "argument" (mirroring `pthread_create()`'s `void *arg`, even without a pthread of this kernel's own) — `process_trampoline` loads it into RDI immediately before the `iretq`, so it arrives as the entry function's own first C parameter with zero special handling needed in userspace (see `user/init.c`'s `second_thread_entry(long arg)`).
- `join_wait` is `child_wait`'s exact shape, one level down: `child_wait` is a *process's* queue, woken by a *child process* exiting, blocking a *parent process*; `join_wait` is scoped to *one process's own threads*, woken by *any thread of that process* exiting, blocking a *sibling thread* of the SAME process. `thread_zombie_locked()` wakes it unconditionally on every thread exit (harmless no-op if nobody's joining) — see §7.4.

**Where a ring-3 thread's user stack actually lives (not a struct field, but the other half of what `thread_create_user()` has to set up that a kthread never needed):** every process's OWN main thread keeps using the same fixed `USER_STACK_VA` it always has; every ADDITIONAL thread gets its own dedicated 1 MiB VA "slot" inside `USER_THREAD_STACK_BASE`, indexed by that thread's own `thread_table[]` slot index — deterministic, so no separate per-process allocator is needed, and automatically unique (`MAX_THREADS` is a single global ceiling). Only `USER_THREAD_STACK_PAGES` (4) pages at the TOP of each 1 MiB slot are actually mapped; the rest of the slot is left unmapped below it as a (generous) guard region, the same philosophy `vmm_alloc_kernel_stack()` already uses for kernel stacks. **Deliberately never explicitly unmapped/freed per-thread** — a joinable thread's stack pages are reclaimed for free, along with everything else mapped in the process, only when the PROCESS itself is eventually torn down (`vmm_destroy_address_space()` already frees every user-half frame unconditionally). This means a long-lived process that creates and joins many short-lived threads over its lifetime accumulates unreclaimed stack pages until it exits — a known, documented simplification (same category as `vmm_alloc_mmio`'s bump-allocated, never-reclaimed VA space), not an oversight; revisit only if a real workload is ever observed to need per-thread stack reclamation before process exit.

### 6.2 Process Control Block — additions built since (✅, Phase C/D; superseded by Phase 4's split, §6.1)

**This section is now historical.** It documents the fields as they were added, incrementally, to the single conflated `struct process` (Phase C priority fields, Phase D parent/child/handle fields) — all of them appear in §6.1's current Phase 4 shape already, split across `struct thread` (priority, aging) and `struct process` (parent/child/zombie/handles) as described there. Kept below unedited as the historical record of *when* each field was added and *why*, per this doc's own "annotate every addition with the phase that added it" discipline.

```c
struct process {
    /* ... fields above, unchanged ... */

    /* Phase C, ✅ built: priority scheduling */
    uint8_t  priority;              // 0 = highest band (§4.2); every new
                                     // process starts at PRIORITY_NORMAL
    uint32_t ticks_since_scheduled; // aging counter, decays priority-starvation

    /* Phase D, ✅ built: parent/child tracking for a real process_wait() */
    struct process *parent;         // who spawned us; NULL = auto-reap on exit
    uint32_t        parent_pid;     // parent's pid AT CREATION TIME -- `parent`
                                     // is a raw pointer into a slot that gets
                                     // recycled after reaping, so `state` alone
                                     // can't tell "still my parent" from "some
                                     // unrelated new process reused the slot"
                                     // (see parent_is_alive(), a real bug this
                                     // caught -- Bug 11's sibling issue, §16)
    struct process *child_list;     // MY live (non-zombie) children, for `ps`
    struct process *child_next;     // sibling link within child_list
    struct process *zombie_head;    // MY exited-but-unclaimed children (I'm
                                     // the parent) -- list head
    struct process *zombie_next;    // if I myself am a zombie linked into
                                     // SOMEONE ELSE's zombie_head: the next
                                     // sibling after me there. Deliberately
                                     // NOT the same field as zombie_head --
                                     // see §16's Bug-11-adjacent discussion
                                     // of why reusing one field for both
                                     // roles is a real corruption hazard
                                     // one level of process-tree depth in.
    struct wait_queue child_wait;   // parent blocks here until ANY child exits

    /* Phase D, ✅ built: ring-3 process handles (docs/ARCHITECTURE.md §3.4/§3.5) */
    struct proc_handle handles[PROC_HANDLE_MAX]; // handle -> pid translation,
                                                  // see PROC_HANDLE_MAX's comment

    /* Phase 4, ✅ built: the thread split -- see §6.1's current shape */
    // struct thread *threads;     // landed as thread_list/live_thread_count, §6.1
};
```

Each addition is annotated with the phase that added it — nothing here was added "because we'll probably need it eventually." `docs/ARCHITECTURE.md` §1 explicitly warns against over-scoping (COW/`fork()` were deliberately pushed off the critical path); this table applies the same discipline field-by-field. One divergence from this section's earlier sketch worth flagging: the original draft proposed a single `zombie_next` field serving as both "list head of my children's exited-but-unclaimed zombies" and "my own link if I'm one of them" — building `process_wait()` for real surfaced that this is unsafe the moment a process tree is more than one level deep (a zombie sitting in its own parent's list, while simultaneously being some OTHER live process's parent, would have its sibling-chain pointer silently clobbered). Split into `zombie_head` (list-head role) and `zombie_next` (sibling-link role) to close that.

### 6.3 Run queue (Phase A/C ✅ built; per-CPU run queues still ⏳ deferred)

Phase A (✅ built, implicit): the run queue *is* the schedulable-unit table, scanned linearly from the current thread's index forward. This was O(n) per `schedule()` call at `MAX_THREADS = 16` (Phases A–4), irrelevant at that size. **(Phase 4 rename, no behavior change: this table is `thread_table[MAX_THREADS]` now, not `proc_table[MAX_PROCESSES]` — see §6.1.)**

Phase C (✅ built, but NOT via the intrusive per-band linked list this section originally sketched): priority scheduling landed as a band-by-band re-scan of the SAME flat table, not a separate `struct run_queue` with per-band `head`/`tail` pointers. `schedule()`'s "find next" loop scans bands `0..SCHED_PRIORITY_BANDS-1` in order, and within each band does the exact same round-robin linear scan Phase A already did, filtered to `candidate->priority == band`. This is O(bands × n) instead of the originally-sketched O(1)-per-band-head — a worst case of 64 comparisons per `schedule()` call at the original `MAX_THREADS = 16`, not worth the extra bookkeeping (inserting into and unlinking from per-band lists on every priority change, block, and wake) that the intrusive-list design would have required.

**Phase 5 update: this got revisited, partially.** `MAX_THREADS` grew 16 → 256 (§13's Phase 5 entry) — exactly the "order of magnitude" both paragraphs above named as the trigger to revisit. It was revisited, but not redesigned: a genuinely per-band intrusive-list run queue is still real bookkeeping work this single-user-workstation OS doesn't clearly need yet, so instead of building it speculatively, the two timing-sensitive SMP selftests that this scan cost could plausibly affect (`test smp sched`, `test thread smp` — both sample "did dispatch happen on ≥2 distinct cores" over a short fixed window) had their sampling windows doubled (`selftest_wait_ticks(20)` → `40`) as cheap insurance, after one of them was observed to fail intermittently under heavy host CPU contention during Phase 5's own verification (4/5 clean reruns, 1 spurious single-core result — a timing artifact of the fixed window, not a scheduling correctness bug; re-verified passing consistently after the wait-tick increase). The underlying O(bands × n) scan itself is unchanged and still not considered a real bottleneck at `MAX_THREADS = 256` under normal (non-contended) conditions — this remains the "revisit only if `g_sched_lock` is *measured* to bottleneck" trade-off (§17), not something Phase 5 concluded needed fixing outright.

Per-CPU run queues (⏳, explicitly deferred — see §8, §13's "explicitly not scheduled" line): one run queue **per CPU**, not one global one. The single global `g_sched_lock` (Phase SMP, §13) is the shipped design; per-CPU queues remain the *next* step only if that lock is *measured* to bottleneck, not a speculative build now. Exact shape (intrusive per-band lists vs. per-CPU flat tables) should be decided against real profiling data if that day comes, not designed ahead of it.

---

## 7. State Machine

```
                    process_create()
                          │  (proc_alloc marks PROCESS_BLOCKED first, Bug 8 §16;
                          │   process_create flips to READY only once fully built)
                          ▼
                  ┌───────────────┐
        ┌────────▶│ PROCESS_READY │◀────────────────┐
        │         └───────┬───────┘                  │
        │                 │ scheduler picks it        │ unblocked
        │                 ▼                            │ (Phase B, ✅: wait_queue_
        │         ┌───────────────┐   blocks on        │  wake_one/_all)
   quantum ends /  │PROCESS_RUNNING│──wait-queue──────▶│
   (✅ timer IRQ)  │               │  (Phase B, ✅)     │
   voluntary yield │               │             ┌──────┴────────┐
        │         └───────┬───────┘              │PROCESS_BLOCKED│
        └─────────────────┘                       └───────┬───────┘
                          │ sys_exit / process_kill                │ process_kill
                          │ (✅ uncatchable, any state)              │ (✅ unlinks
                          ▼                                          │  from queue)
                  ┌───────────────┐◀────────────────────────────────┘
                  │PROCESS_ZOMBIE │
                  └───────┬───────┘
                          │ automatic (✅ Phase A/B, §7.4) if no live
                          │ parent; OR handed off to parent_wait (✅
                          │ Phase D) — process_wait()/sys_wait genuinely
                          │ block, no more busy-polling
                          ▼
                  ┌───────────────┐
                  │PROCESS_UNUSED │  (slot reclaimed: pml4 destroyed,
                  └───────────────┘   kstack freed, PCB zeroed)
```

**Invariants (violate any of these → kernel panic, never silent continuation):**

1. Exactly one PCB has `state == PROCESS_RUNNING` at a time, per CPU.
2. `current_process` is never `NULL` while any PCB is `RUNNING` (today: `schedule()` returns early if `current_process == NULL`, which is correct only because nothing calls `schedule()` before `process_start_first()` — this coupling is implicit and should become an explicit assertion once more entry points exist, Phase B).
3. A `PROCESS_ZOMBIE` PCB's address space and kernel stack are **never touched again** by the scheduler (they're pending reclamation, not usable) but the PCB slot itself must **not** be reused until reaped — reusing it early would let a stale `pid` alias a live process, which is exactly the kind of bug that turns into a security hole (PID reuse races are a known real-world class — Linux's `PIDTYPE_PID` + `pid` namespace churn exists partly to manage this).
4. `PROCESS_BLOCKED` (Phase B, ✅) is only ever entered and exited through the wait-queue primitive (§7.3) — never set directly by ad hoc code, or the state machine above is no longer complete and every diagram/invariant here is void. (One narrow, deliberate exception: `proc_alloc()` sets a *brand-new* slot to `PROCESS_BLOCKED` before it's schedulable at all, Bug 8 §16 — this is initialization, not a wait-queue transition, and the slot is never linked into any `struct wait_queue`, so invariant 4's actual concern — "don't let ad hoc code fake being blocked-on-something" — still holds.)

### 7.1 PID allocation

Currently a bare monotonic counter (`next_pid++`, wraps only at `uint32_t` overflow — effectively never in practice, but not formally handled). This is fine indefinitely for a single-user desktop OS; Linux's `pid_max` wraparound-with-reuse machinery exists to bound `/proc` and signal-delivery races under high process churn, which are not primary use cases here per `docs/ARCHITECTURE.md`'s message-port model. **Revisit only if PID reuse under churn becomes an observed problem, not preemptively.**

### 7.2 Dispatch ordering (why CR3/RSP0 happen before the context switch)

This is already correct in the tree (`schedule()` and `process_start_first()` both do it in this order) but is worth specifying explicitly because getting the order wrong is a classic, silent, timing-dependent bug:

```
1. vmm_switch_address_space(next->pml4_phys)   // CR3 now points at next's tables
2. tss_set_rsp0(next->kstack_top)              // TSS.RSP0 now correct for next
3. kernel_ctx_switch(&prev->ctx, &next->ctx)    // registers/RSP/RIP swap; may not return here for a while
```

**Why this order and not the reverse:** between steps 1–3, execution is still physically running on `prev`'s kernel stack (the context switch in step 3 hasn't happened yet). If an interrupt landed in that window, the CPU would push the trap frame using **whatever `TSS.RSP0` currently says** — so `RSP0` must already describe `next`'s stack, not `prev`'s, the instant `CR3` changes (steps 1 and 2 are actually a single atomic-in-spirit unit; doing them in either order relative to *each other* is fine since interrupts are typically disabled across this whole sequence today, but flipping the pair relative to step 3 is not fine). The kernel-half of the address space is identical across all processes (`docs/ARCHITECTURE.md` §3.1's shared-kernel-space assumption), so continuing to execute kernel code immediately after the `CR3` write is safe — that shared-mapping property is precisely what makes this ordering *legal*, and is worth remembering as a dependency if the VMM's kernel-mapping strategy ever changes.

### 7.3 Wait queues (Phase B, ✅ built)

A blocked process is removed from the run queue entirely (not skipped-in-place — an O(n) scan that skips blocked processes every quantum is wasted work once queues are non-trivial, and more importantly it's the wrong *shape*: a blocked process isn't "not its turn yet," it's "not eligible," and those are different facts the scheduler needs to reason about differently once priority aging (§4.2 Phase C) exists — an ineligible process must not accumulate aging credit).

```c
struct wait_queue {
    struct thread *head;   // Phase 4: intrusive via thread::wait_next (was
                           //   process::wait_next -- threads, not processes,
                           //   are what actually blocks/wakes, §6.1)
};

void wait_queue_block(struct wait_queue *wq, struct thread *t);  // READY/RUNNING -> BLOCKED
void wait_queue_wake_one(struct wait_queue *wq);                  // BLOCKED -> READY, one thread
void wait_queue_wake_all(struct wait_queue *wq);                  // BLOCKED -> READY, all
```

Built exactly as specified — `process.c`'s `wait_queue_block`/`wait_queue_wake_one`/`wait_queue_wake_all`, plus a static `wait_queue_remove` helper used both by a normal wake and by `process_kill` (§9) unlinking a killed-while-blocked target. `thread::wait_queue` (§6.1) records which queue a blocked thread is currently on, so `process_kill` doesn't need the caller to already know. **Phase 4 update:** the queue holds `struct thread *`, not `struct process *`, now that the two are split — a process with multiple threads can have some blocked and some running at once, which the pre-split shape had no way to express even though nothing exercises it yet (every process still has exactly one thread outside `test thread smp`/`exit`).

This is deliberately *not* a condition variable in the pthreads sense (no associated mutex to atomically release-and-block) — under SMP (✅ built, Phase SMP below) "atomically" is provided by `g_sched_lock` being held across the block-then-`schedule_locked()` sequence, not by interrupts-off alone anymore (see §8(b)). **Now genuinely exercised by something user-facing**: `process_wait()`/`sys_wait` (§7.4) blocks the caller on the target's parent's `child_wait` queue for real, woken by the target's own exit or kill — no more busy-polling. Getting this exercised for the first time immediately surfaced Bug 11 (§16): `schedule()`'s zombie hand-off ran too late to ever wake a parent sitting `BLOCKED` here with nothing else runnable, a real deadlock, fixed by reordering `schedule()` itself (see §8(a) and Bug 11's full writeup).

### 7.4 Termination and reclamation (✅ all built — automatic reap Phase A, pid-directed `process_reap` Phase B, real parent-blocked `process_wait()`/`sys_wait` now Phase D — closed Bug 3 and Bug 11, §16)

Automatic reclamation (Phase A shape below is historical — updated after for the Phase SMP per-core deferral and the Phase 4 thread/process split):

```
sys_exit(code):
    current_process->exit_code = code
    current_process->state = PROCESS_ZOMBIE
    schedule()   // never returns

schedule():                              // (excerpt — the reclaim-relevant part)
    if g_pending_reap:                   // zombie from the PREVIOUS schedule() call
        process_reap_slot(g_pending_reap)  // safe now: definitely off that stack
        g_pending_reap = NULL
    ... pick next, as before ...
    if prev->state == PROCESS_ZOMBIE:
        g_pending_reap = prev            // reclaim next time, not now — see §7.2:
                                          // we're still on prev's stack until the
                                          // kernel_ctx_switch below actually runs
    kernel_ctx_switch(&prev->ctx, &next->ctx)

process_reap_slot(proc):
    assert(proc->state == PROCESS_ZOMBIE)
    vmm_destroy_address_space(proc->pml4_phys)
    vmm_free_kernel_stack(proc->kstack_top, KSTACK_SIZE)   // not kfree — see Bug 4, §16
    memset(proc, 0, sizeof(*proc))
    proc->state = PROCESS_UNUSED
```

**Phase 4 shape (✅ built — the actual current code), two-level and per-core:**

```
process_exit_self(code):
    current_process->exit_code = code
    current_thread->state = PROCESS_ZOMBIE
    schedule()   // never returns

schedule_locked():                       // per-core, this_cpu()->pending_*_reap
    if this_cpu()->pending_thread_reap:  // thread from THIS core's PREVIOUS schedule_locked() call
        thread_reap_slot(this_cpu()->pending_thread_reap)   // frees kstack only
        this_cpu()->pending_thread_reap = NULL
    if this_cpu()->pending_process_reap: // only set if that thread ALSO completed its process
        process_reap_slot(this_cpu()->pending_process_reap) // frees the address space
        this_cpu()->pending_process_reap = NULL
    ... pick next, as before (now scanning thread_table) ...
    if current_thread->state == PROCESS_ZOMBIE:
        dying_process = thread_zombie_locked(current_thread)  // decrements
            // proc->live_thread_count; returns the process ONLY if this was
            // its LAST thread AND there's no live parent to hand off to
    ... (if a real switch is about to happen) ...
    // Phase 5 exception: a JOINABLE thread (thread_create_user()) that is
    // NOT also completing its process (dying_process == NULL, i.e. it had
    // live siblings) is left OUT of the reap queue -- its stack must
    // survive for thread_join() to collect later (§6.1's Phase 5 addendum).
    if !(current_thread->joinable && dying_process == NULL):
        this_cpu()->pending_thread_reap = current_thread   // (the outgoing thread)
        this_cpu()->pending_process_reap = dying_process   // (NULL unless the process also completed)
    kernel_ctx_switch(&prev->ctx, &next->ctx)

thread_reap_slot(t):
    assert(t->state == PROCESS_ZOMBIE)
    vmm_free_kernel_stack(t->kstack_top, KSTACK_SIZE)
    memset(t, 0, sizeof(*t)); t->state = PROCESS_UNUSED

process_reap_slot(proc):                 // unchanged in spirit from Phase A/B
    vmm_destroy_address_space(proc->pml4_phys)
    memset(proc, 0, sizeof(*proc)); proc->pid = 0
```

The generalization from Phase A/B: reclaiming a THREAD's kernel stack still can't happen inside the same `schedule_locked()` call that switches away from it (same reasoning as before — we're still executing on that stack until `kernel_ctx_switch` runs), so the one-pending-slot deferral is unchanged in spirit, just per-core (Phase SMP) and now split into a thread-level slot (freed every time a thread exits) and a process-level slot (freed only when that thread was also the LAST thread of its process). `thread_zombie_locked()` is the shared decision point — used by both `schedule_locked()`'s self-exit path and `process_kill()`'s external-kill path — and is deliberately idempotent (safe to call more than once for the same thread) because `schedule_locked()`'s pre-existing retry behavior (§7.2/§11's "nothing else runnable yet" case) can re-run the same ZOMBIE-handling block on a later tick; idempotence is achieved by unlinking the thread from `proc->thread_list` as the FIRST thing `thread_zombie_locked()` does, so a second call finds it already unlinked and skips the decrement/hand-off entirely. **Now genuinely exercised**: `test sched reap` (still exercises the original single-thread path), plus the two new Phase 4 tests — `test thread smp` (3 threads, 1 process, proves the address space isn't reaped while ANY thread is still alive) and `test thread exit` (proves it's reaped exactly once the LAST one exits, not the first).

**Built in Phase B:** `process_reap(pid)` — looks up the PCB by pid and calls `process_reap_slot` on it if it's a zombie.

**Built in Phase D:** real parent-blocked termination collection, replacing the earlier busy-poll entirely.

```
process_wait(pid):                       // process.c -- used by both sys_wait and
                                          // the kernel shell's `wait` command (main.c)
    loop:
        walk current_process->zombie_head looking for `pid`
        if found:
            unlink it, read its exit_code, process_reap_slot() it, return the code
        target = process_find(pid)
        if !target or target->parent != current_process:
            return -EMBK_ECHILD          // not ours to wait for
        wait_queue_block(&current_process->child_wait, current_thread)  // Phase 4: queues
                                           // the THREAD, not the process (§6.1/§7.3)
        schedule()                        // resumes once SOME child of ours exits;
                                           // loop back and re-check -- might not be
                                           // this pid yet if we have >1 child

# On the exiting side (thread_zombie_locked(), shared by schedule_locked()'s
# self-exit path and process_kill()'s external-kill path -- Phase 4, §7.4):
    if parent_is_alive(dying_process):
        dying_process->zombie_next = dying_process->parent->zombie_head
        dying_process->parent->zombie_head = dying_process
        wait_queue_wake_one(&dying_process->parent->child_wait)
    else:
        return dying_process              // caller defers the actual reap -- §7.4
```

`process::parent`/`parent_pid` (set once, at creation, in `process_create()`) is how the exiting side knows *whether* anyone will ever collect it, and `parent_is_alive()` (§16) guards against a stale `parent` pointer aliasing an unrelated process that later reused the same recycled PCB slot. `sys_wait(handle)` (`syscall.c`) is now a thin wrapper: resolve the handle to a pid (§15.2), call `process_wait(pid)`, free the handle. The kernel's own interactive shell (`main.c`) calls `process_wait(pid)` directly with no handle indirection at all, since it's trusted kernel code, not a sandboxed ring-3 caller.

Getting this blocking path exercised for the very first time (`test sched wait`) immediately hung — Bug 11 (§16): the zombie hand-off above was originally reachable only *after* `schedule()` had already confirmed some other runnable process existed, but the whole point of `process_wait()` is a parent sitting `BLOCKED` (not READY/RUNNING) with nothing else runnable at that exact moment. Fixed by moving the hand-off decision before that check.

---

## 8. Synchronization

**Before Phase B:** none needed. Single core, and `schedule()` was only ever called from syscall context, never from an interrupt handler, so there was no reentrancy to guard against.

**Since Phase B (✅ preemption is real now):** two separate hazards, worth keeping distinct — (a) is now ✅ **fully closed**; (b) is still purely Phase D/future.

- **(a) Preemption reentrancy — ✅ closed.** When `schedule()` is entered *from the LAPIC timer ISR*, it was always safe from being reentered by a second timer tick: the timer handler is an interrupt gate (`kcontext.asm`'s callers all are), so IF is 0 for its entire duration and no further timer IRQ can land until it returns — this is why Bug 6/7's fix (forcing IF=1 only in the *saved snapshot*, not in the live flags during the ISR) doesn't reopen this hole. **But** `schedule()` is also called from syscall context (`sys_exit`/`sys_yield` → `process_exit_self`/`sys_yield` → `schedule()`), and `syscall_dispatch()` unconditionally executes `sti` before dispatching (needed for Bug 7's fix, disk-I/O syscalls). That meant a `schedule()` call triggered by `sys_exit`/`sys_yield` ran with IF=1 — a timer IRQ *could* land mid-dispatch inside that call and re-enter `schedule()` from the timer ISR while the outer, syscall-triggered `schedule()` call hadn't returned. **Fixed:** `schedule()` now does the standard thing itself — `pushfq`/`pop`/`cli` at entry (same idiom as `cpu/spinlock.c`'s `spin_lock`), saving the caller's original IF and unconditionally disabling interrupts for the scan/dispatch section; every early-return path restores the saved IF before returning, and the path that reaches `kernel_ctx_switch` doesn't need to restore anything explicitly — the switch itself always resumes with IF=1 forced (Bug 6), regardless of what was saved. `schedule()` is now self-contained and safe to call from *any* interrupt state, not just the two states it happens to be called from today. Verified: `test sched roundrobin`/`kill`/`reap`/`stackguard` all still pass after the change (rerun in QEMU), including the two tests that most directly stress this exact path — kthreads exit via `process_exit_self` (the same call shape as `sys_exit`, IF=1) while the timer is actively preempting them throughout.
- **(b) Cross-CPU run-queue access — ✅ closed (SMP phase, §13).** Real cores exist now (`-smp 4` verified). One global `g_sched_lock` (a real `cpu/spinlock.c` spinlock) guards every `thread_table`/`process_table`/`current_thread`/`pending_*_reap` access (Phase 4 rename: was `proc_table`/`current_process`/`pending_reap`, §6.1), and — the part that is *not* just "add a lock" — it is **held across `kernel_ctx_switch` itself**, released only by whichever context resumes on the far side (the line after the switch, or a first-dispatch trampoline). Releasing it any earlier, even for one instruction, lets another core observe a mid-transition PCB as schedulable and dispatch it while it is still physically executing — every variant of that window was hit as a real crash during bring-up, not hypothesized (see Bugs 12–24, §16). `schedule()`'s Bug-10-era `pushfq`/`cli` prologue was replaced by the lock (same interrupt guarantee, now also cross-core).

**Design decision (originally "for Phase D," now partially superseded by what actually shipped):** the single global lock above is the shipped design, exactly as the "one lock first, measure before sharding" plan of record said. Per-CPU run queues, each with their own spinlock, remain the *next* step **only if the global lock is measured to bottleneck**. Reasoning preserved for that day:

- A single global lock turns the run queue into a serialization point across every CPU on every scheduling decision — exactly the bottleneck Linux moved away from decades ago (the O(1) scheduler and later CFS are both partly *about* per-CPU run queues to avoid this).
- Per-CPU queues need a **load-balancing** story (what stops one CPU's queue from starving while another's overflows) — this is real, non-trivial work, which is exactly why it's scoped to Phase D and not attempted speculatively now. Building the per-CPU *shape* early (§6.3) costs nothing; building load-balancing early would be solving a problem that doesn't exist on one core.
- The uncatchable kill (§9) is the one operation that must work *across* CPUs even in a per-CPU-queue world (killing a process running on a different CPU) — flagged here because it's the one place Phase D's design must explicitly revisit Phase A's single-CPU assumption, not an afterthought.

---

## 9. Security Considerations

- **Uncatchable kill is a scheduler-level guarantee, not a userspace-observable one.** It must not go through the message-port queue at all (a process that refuses to drain its queue must still die) — implementation is a direct state transition (`RUNNING`/`READY`/`BLOCKED` → `ZOMBIE`) forced by the kernel, bypassing any process-side handling entirely. This is `docs/ARCHITECTURE.md` §3.3's requirement; the scheduler is where it's actually enforced. **✅ Built:** `process_kill(pid)` (`process.c`) does exactly this — forces `ZOMBIE` from any of `RUNNING`/`READY`/`BLOCKED`, unlinks from a wait queue if blocked, reaps immediately unless the target is `current_process` (deferred one `schedule()` call, same reasoning as §7.4's normal-exit path). Verified via `test sched kill`.
- **Address-space isolation is the only isolation that exists today**, and it's only as strong as the VMM underneath it (separate PML4s, no shared user-writable mappings). This subsystem does not add isolation on top of that — it *relies* on it being correct. Worth stating explicitly: a scheduler bug that runs the wrong process's `ctx` against the *previous* process's still-loaded CR3 (an ordering bug — see §7.2) is a cross-process memory-disclosure bug, not just a crash. This is why §7.2's ordering is specified as an invariant, not a suggestion.
- **`current_process` as a bare global pointer is a confused-deputy risk once syscalls read it implicitly** (e.g., `sys_exit`/`process_exit_self` reads `current_process->exit_code` directly). This is safe *only* as long as every syscall handler is entered with `current_process` already correctly pointing at the caller — true today because there's exactly one core, but reentrancy is no longer hypothetical: real preemption (Phase B, ✅) means a timer IRQ can now land mid-syscall and call `schedule()`, which *does* mutate `current_process`. This has not yet caused an observed bug (the timer handler doesn't read/write `current_process` itself outside of `schedule()`'s own dispatch, and `schedule()` was already the sole writer), but it is a materially different risk profile than "true today because... no reentrancy" was when this line was first written — worth re-auditing before SMP (Phase D) adds a second, genuinely concurrent reader/writer.
- **User-pointer validation is now built, one layer up, exactly where this section said it belonged.** `cpu/usercopy.c/h`'s `access_ok`/`copy_from_user`/`copy_to_user`/`copy_string_from_user` guard every syscall that takes a user buffer or path (`sys_write`, `sys_read`, `sys_open`, `sys_stat`, `sys_readdir`, `sys_spawn`'s path argument). This subsystem still does not implement or own that check — noted here only to close the loop on the previous version of this line, which flagged it as an open problem tracked in `cpu/syscall.c`.
- **Kernel stack guard pages (Bug 4, §16) are a security boundary, not just a robustness one** — an unguarded stack overflow that silently corrupts the adjacent heap allocation is a real privilege-preserving memory-corruption primitive if a hostile (or just buggy) userspace process can control its own stack depth (recursion, alloca-heavy code) enough to walk off the end. This is why it's listed as a **Failure Mode** (§10) *and* here.
- **`sys_thread_create`'s `entry_point` is a ring-3-supplied arbitrary address, unlike anything else this subsystem hands to the scheduler** — every other entry point the scheduler ever fabricates a context around (`process_create()`'s ELF entry, every kthread's function pointer) is chosen by trusted kernel code, not by the ring-3 caller directly. `sys_thread_create` (cpu/syscall.c, Phase 5) validates it with `access_ok()` before ever calling `thread_create_user()` — the same check every user-buffer-taking syscall already applies (the bullet above), extended here to a value that's used as a jump target rather than dereferenced as data. Without it, a caller could point a brand-new thread's very first instruction at unmapped or kernel-half memory before the thread ever runs, rather than failing cleanly at `sys_thread_create()`'s own call site.

---

## 10. Failure Modes

| Failure | Behavior before the fix | Now |
|---|---|---|
| Kernel stack overflow (Bug 4) | Silent heap corruption, manifests later as an unrelated-looking crash | ✅ Fixed. Unmapped guard page below every kernel stack → immediate `#PF` at the overflow site |
| Kernel-stack VA region invisible in a fresh process's own page tables (Bug 5) | N/A — introduced *by* the Bug 4 fix, in the same session; never shipped independently | ✅ Fixed. Region reuses `MMIO_BASE`'s already-populated PML4 slot instead of a fresh one — see §7.2/§16 |
| Zombie accumulation (Bug 3) | After exactly `MAX_PROCESSES` exits, `process_create` fails forever until reboot | ✅ Fixed (automatic reclamation, §7.4), now genuinely exercised by `test sched reap`. Real parent-blocked `process_wait()` also built (§7.4); orphan-collecting PID 1 is not (no init/service-manager process exists yet at all — `docs/ARCHITECTURE.md` §7 Userspace runtime, unrelated to this subsystem) |
| Runaway process (infinite loop, ignores its message port) | Previously *couldn't* be stopped — no preemption existed, so it never yielded the CPU at all | ✅ Fixed. Timer-driven preemption (§5, ✅) means it's evicted every quantum regardless of cooperation, and the uncatchable kill (§9, ✅) can force it to `ZOMBIE` outright — verified together via `test sched kill` (kills a process running an intentional infinite loop) |
| Double-free / use-after-reap | N/A, reaping didn't exist | Guarded now: `process_reap_slot` asserts `state == PROCESS_ZOMBIE` before freeing anything; reused PCB slots are always fully zeroed first |
| Scheduler picks a half-initialized `PROCESS_READY` PCB (Bug 8) | Possible the instant real preemption + a second `process_create()` call coexist — `proc_alloc()` marked slots `READY` immediately, before pml4/kstack/ctx were built | ✅ Fixed. `proc_alloc()` marks `PROCESS_BLOCKED` instead; only `process_create()`'s final step sets `READY`, after everything is actually built |
| Scheduler picks a `BLOCKED` process | Previously not possible (nothing set `BLOCKED`) | ✅ Structurally impossible now that §7.3 is built: the run-queue removal on block (§7.3's design rationale) means a blocked PCB simply isn't in the schedulable set, not "checked and skipped at schedule time" |
| RFLAGS.IF corrupted across a context switch (Bug 6) | N/A, no preemption existed to expose it | ✅ Fixed — `kernel_ctx_switch`/`kernel_ctx_save` force IF=1 in the saved snapshot (see Bug 6, §16) |
| Syscall hangs waiting on an IRQ that can't fire (Bug 7) | N/A, no syscall did blocking I/O before file-I/O syscalls landed | ✅ Fixed — `sti` at the top of `syscall_dispatch()` |
| PCB slot leaked on process_create() failure (Bug 9) | A failed creation (bad ELF, OOM) permanently removed a slot from the pool, with no PID ever having existed to explain why | ✅ Fixed — all four early-return error paths reset `state = PROCESS_UNUSED` |
| Timer IRQ re-enters `schedule()` while a syscall-triggered `schedule()` call is still in progress (§8(a)) | N/A, no preemption existed | ✅ Fixed. `schedule()` now `cli`s at entry (saving the caller's IF) and restores it on every early-return path — see §8(a)/Bug 10, §16 |
| Two CPUs both believe they're running the same PCB | Actually happened during SMP bring-up, twice, from different causes (Bugs 13 and 22, §16) | ✅ Fixed. `g_sched_lock` held across the switch, the scan's `candidate == current_process && RUNNING` self-fallback (never a *different* core's RUNNING process), the `running_cpu` owner field, and `process_init()` ordered before `smp_bringup()` |
| A core keeps executing a stack another core just freed (zombie reap race) | Actually happened: #DF inside `vmm_switch_address_space` — the CR3 reload flushed the stale TLB entries that were the only thing keeping the freed stack readable (Bugs 19/23, §16) | ✅ Fixed. Dying core clears `running_cpu` + switches away inside the same `g_sched_lock` hold that posts the hand-off; per-core pinned idles guarantee that switch always has a target; `process_wait()` keeps a `running_cpu` guard as belt-and-suspenders |
| 🚧 **`sys_exit()` called via `enter_user_mode()`'s standalone launch (`test ring3`, `cpu/usermode.c`) corrupts the CALLING thread's own scheduling state, not the ring-3 program's** | Pre-existing since `process_exit_self()` was first built (Phase B) — `enter_user_mode()` never creates a real `struct thread`/`struct process` of its own; it's a one-shot CR3-switch-and-`iretq` entirely outside the scheduler. `sys_exit`'s `process_exit_self()` unconditionally acts on `current_thread`/`current_process`, which at that point is still whichever thread CALLED `enter_user_mode()` (in practice, the interactive shell) — so the ring-3 program's own `exit()` marks the SHELL's thread `ZOMBIE` and hands it to `schedule()`, which switches away and never resumes the shell's `main.c` loop. **Found, not fixed, while verifying Phase 5** (confirmed present with the plain pre-Phase-5 `init.c` too, via a controlled A/B rerun — this is not something Phase 4 or 5 introduced). The properly-scheduled path (`process_create()`/`process_wait()`, what `test ring3 threads` and the shell's own `run`/`wait` commands use) does not have this problem: a child's `sys_exit()`/`sys_thread_exit()` there always acts on the CHILD's own `current_thread`, never the caller's, because `schedule()` genuinely switched context to it first. **Out of scope for this phase** — `test ring3` predates and is unrelated to the process/thread split or ring-3 threads; flagged here for whoever next touches `enter_user_mode()` or `test ring3`, not addressed by this work. |

---

## 11. Debugging Strategy

- The existing serial exception dump (used this session to diagnose Bug 1) prints vector, error code, `RIP`, `CS`, `RSP`, `SS`, `RBP` — sufficient to catch a bad `iretq` frame, as it did. **Extension for this subsystem:** on any exception where `CS` indicates ring 0 (a kernel-mode fault, like Bug 1 was — the fault happens *during* the privilege transition, still ring 0), additionally dump `current_process->pid` and `state`, so a future "which process's trampoline just crashed" question doesn't require a `gdb` session to answer from serial logs alone.
- `docs/GDB_CHEATSHEET.md` already documents the QEMU `-s -S` + `gdb` workflow (`make debug` target) — the natural tool for stepping through `kernel_ctx_switch`/`process_trampoline`, since bugs in this subsystem are exactly the kind (wrong register in wrong slot, wrong selector value) that a register dump at the fault site resolves in minutes, as it did for Bug 1 this session.
- **Proposed addition:** a `dump_process_table()` debug command (following the existing `selftests.c` `test <name>` pattern) that prints every PCB's `pid`/`state`/`priority` — the single most useful "is the scheduler stuck" diagnostic, and trivial to build once §7 lands.

---

## 12. Testing Strategy

Following the existing `selftests.c` convention (`test embkfs`, `test vfs`, `test fd`, `test ring3`, etc. — a shell command dispatches to a self-contained in-kernel check that prints pass/fail with a concrete assertion, not just "didn't crash"). **Ten fully in-kernel selftests are built** (`process_test_*` in `process.c`, wired into `selftests.c` as `test sched roundrobin`/`kill`/`reap`/`stackguard`/`wait`/`priority`, `test smp sched`/`kill`, `test thread smp`/`exit`), closing the gap this section originally flagged (everything through Bug 5 was manual QEMU + `gdb`/`objdump` only), **plus one selftest that needs a real disk** (`test ring3 threads`, Phase 5, below). All eleven pass together, in order, under `-smp 4` — the standard verification pass for any change to this subsystem:

- **`test sched roundrobin`** ✅: uses `process_create_kthread` (ring-0 "threads" sharing the kernel's own PML4, built specifically so scheduler tests don't need a real ELF file) to spawn dummy processes that each increment a shared counter; asserts every process ran at least once within N scheduling rounds and none ran twice before all others ran once. Also exercises the wait-queue blocking path (§7.3).
- **`test sched kill`** ✅: spawns a kthread running an intentional infinite loop that never touches its message port; issues `process_kill`; asserts the PCB reaches `PROCESS_ZOMBIE` within one scheduling quantum. This is the test that actually proves §3.3's "day one" guarantee is real, not aspirational.
- **`test sched reap`** ✅: spawns and exits a process well past `MAX_PROCESSES` in a loop; asserts this keeps succeeding (proves Bug 3 stays fixed — a regression here is exactly "silent until it isn't," the failure mode this whole spec is trying to design against). This is also the first test to actually *exercise* the deferred-reap path (§7.4) — before it existed, nothing in the tree had ever created a second concurrent process to trigger it.
- **`test sched stackguard`** ✅: deliberately checks that the guard page below a kernel stack is genuinely unmapped (`vmm_get_phys` returns not-present) rather than actually recursing into it — there's no fault-recovery path yet, so the test proves the guard page *would* fault without needing to survive a real one.
- **`test sched wait`** ✅: exercises `process_wait()`'s two zombie hand-off paths (normal exit, uncatchable kill) — see §7.4.
- **`test sched priority`** ✅: exercises priority bands + aging (§4.2 Phase C) — a `PRIORITY_REALTIME` kthread must dominate a `PRIORITY_BACKGROUND` one busy-looping alongside it, but aging must eventually let the background one make *some* progress too.
- **`test smp sched`** ✅ (Phase SMP): spawns 8 kthreads, each recording `lapic_get_id()` on first execution; asserts ≥2 distinct core IDs — proves real concurrent cross-core scheduling, not just time-slicing on one core.
- **`test smp kill`** ✅ (Phase SMP): spawns a busy-loop kthread, waits until it's observed `RUNNING` on a *different* core than the caller, `process_kill()`s it from here; asserts its counter stops advancing and its slot is eventually reaped — exercises `process_kill()`'s running-elsewhere branch specifically (§9).
- **`test thread smp`** ✅ (Phase 4; sample size raised Phase 5 — see below): one process (`process_create_kthread`), seven *additional* threads under it (`thread_create`) — eight threads total (was three, at Phase 4 ship), all sharing the same `proc->pml4_phys`. Asserts all eight first-execute (≥2 distinct cores) AND all genuinely increment the SAME shared counter — the second assertion is what actually distinguishes "one shared address space" from "eight processes that coincidentally share a pid," since a shared *heap variable* being consistently incremented by threads running on different cores can't happen with separate address spaces. **A real test-flakiness finding from Phase 5's own verification, not a kernel bug:** at the original sample size of 3, "all N first-executions land on the same core" was observed in about 1 run in 4 under heavy host CPU contention — and it is NOT independent per-thread chance (which would predict under 1% at N=8): these threads all become READY back-to-back in one tight loop, so a single core's own successive ticks can round-robin through every one of them in a streak before the OTHER core's next tick happens to land at all if the host is starving that core's vCPU thread of real time. Raising the sample to 8 (matching `test smp sched`'s already-reliable count) and widening `selftest_wait_ticks()` (20 → 100) both helped — observed failure rate dropped from roughly 1-in-4 to roughly 1-in-8 runs under the same adverse conditions — but did not eliminate it outright, which is itself the signature of a correlated/bursty failure mode rather than an independent one. See `THREAD_SMP_COUNT`'s comment (process.c) for the full account.
- **`test thread exit`** ✅ (Phase 4): one process, two threads (A exits almost immediately, B keeps spinning); asserts the process is still alive (`live_thread_count == 1`, findable via `process_find`) while B is still running, and is only reaped once B *also* exits — proves the address space survives until the LAST thread exits, not the first, the core claim the whole split exists to make true.
- **`test ring3 threads`** ✅ (Phase 5): the one selftest in this list that ISN'T purely in-kernel (kthreads) — it needs a real filesystem with `/init.elf` on it (e.g. `make run-embkfs`), because exercising `sys_thread_create`/`sys_thread_join` genuinely needs a ring-3 process, unlike every test above (which use `process_create_kthread`'s ring-0 shortcut specifically so they don't need one). Goes through the REAL scheduler (`process_create()`/`process_wait()`, the same path `sys_spawn`/`sys_wait` and the shell's own `run`/`wait` commands use) rather than the standalone `enter_user_mode()` path `test ring3` uses, because `thread_join()`'s blocking path needs a genuinely scheduled process. `/init.elf` itself (`user/init.c`) spawns a second thread of itself, joins it, and checks that the second thread's writes to `.data` (`counter`, `thread_ran`) are visible back in the main thread after the join — the exit code (16) only comes out right if thread creation, the join, AND the shared-memory check all succeeded; any failure exits with -1 instead.

---

## 13. Implementation Plan / Roadmap

Phased by dependency, matching the granularity of `docs/ARCHITECTURE.md` §5's project-wide roadmap (this is the detailed breakdown of that roadmap's item 3, "Scheduler").

**Phase A — Correctness of what exists (✅ complete):**
1. ✅ Fix Bug 3 (zombie reclamation) — `process_reap_slot`, automatic, so `MAX_PROCESSES` isn't a hard ceiling on total process creations ever. Now genuinely exercised (`test sched reap`, Phase B below).
2. ✅ Fix Bug 4 (kernel-stack guard page) — page-mapped stack with one unmapped page below it, via `vmm_alloc_kernel_stack`.
3. ✅ Fix Bug 5 (found while verifying #2) — the new kernel-stack VA region shared `MMIO_BASE`'s PML4 slot instead of a fresh one, to be visible in every process's page tables regardless of creation order.
4. ✅ `test sched roundrobin` and `test sched stackguard` (§12) — closed the "manual QEMU verification only" gap.

**Phase B — Blocking + preemption (✅ complete):**
1. ✅ Wait queues (§7.3) — `wait_queue_block`/`wait_queue_wake_one`/`wait_queue_wake_all`.
2. ✅ Timer-driven preemption: `lapic_timer_handler` calls `schedule()` on every tick (100Hz). Safe from being reentered by another timer tick (interrupt gate keeps IF=0 for the ISR's duration), and now also safe against the syscall-context reentrancy gap this same phase's work exposed — see §8(a)/Bug 10, §16.
3. ✅ Uncatchable kill (§9) — `process_kill(pid)`. This is the point at which `docs/ARCHITECTURE.md` §3.3's "ships with the scheduler" promise is actually redeemed, not just documented.
4. ✅ `test sched kill` (§12).
5. ✅ `test sched reap` (§12) — pulled forward from the original Phase D plan below once it became clear `test sched roundrobin`'s kthread infrastructure made it trivial to also spawn/exit past `MAX_PROCESSES` in a loop; no reason to wait for full lifecycle completion just to prove Bug 3 stays fixed.
6. ✅ `sys_wait`/`sys_spawn`/`sys_yield`/`sys_getpid` syscalls, per-process fd table (unblocks `spawn()`'s file-action model, §3.2) — also pulled forward from Phase D: once preemption and wait queues existed, a busy-polling `sys_wait` (§7.4) was simple enough to build immediately rather than wait for parent/child tracking. The real blocking version landed shortly after, in the Phase D work below.
7. ✅ Four new bugs found and fixed while building the above (Bugs 6–9, §16): RFLAGS.IF corruption in context switch, `int 0x80` leaving IF=0 for the whole syscall, the `proc_alloc()` READY-before-initialized race, and PCB leak on `process_create()` error paths.
8. ✅ Bug 10 (§16): `schedule()` itself was reentrant against the timer ISR when called from syscall context — found and fixed by re-reading this same Phase B work, not by a crash.

**Phase C — Priority scheduling (✅ complete):**
1. ✅ Fixed priority bands + aging (§4.2, §6.2/§6.3) — `PRIORITY_REALTIME`..`PRIORITY_BACKGROUND` (4 bands), `PRIORITY_AGE_TICKS = 20` (~200ms/band — see §6.2's comment on why this is deliberately short, not the "obvious" multi-second value).
2. ✅ `schedule()`'s "find next" scan now goes band-by-band (0 = highest) before round-robining within a band — see §6.3 for why this shipped as a re-scan of the flat table rather than the originally-sketched intrusive per-band run queue.
3. ✅ `test sched priority` (§12).

**Phase D — Lifecycle completion + ring-3 process handles (✅ complete except SMP shape):**
1. ✅ Real blocking `process_wait()`/`sys_wait` — replaced the busy-poll (§7.4) with `process::parent`/`parent_pid`/`zombie_head`/`zombie_next`/`child_wait` tracking (§6.2), so the exiting child wakes its waiting parent directly. Also built `child_list`/`child_next` (live children, for `ps`).
2. ✅ Ring-3 process handles (§3.4/§3.5, §15.2) — `sys_spawn` returns one, `sys_wait`/`sys_kill` take one, translated via each process's own `handles[PROC_HANDLE_MAX]` table. Closes the raw-pid confused-deputy gap §17 used to flag here.
3. ✅ `sys_kill` — the last piece of the §15.2 syscall surface; `process_kill()` itself has existed since Phase B.
4. ✅ `test sched wait` (§12).
5. ✅ Bug 11 (§16): building `process_wait()` immediately surfaced a real deadlock in `schedule()`'s zombie hand-off ordering — found by the new test hanging on its first run, fixed by reordering.
6. ✅ The kernel's own interactive shell (`main.c`) now calls `process_adopt_current()` to become a real, permanently-scheduled process instead of a one-way `process_start_first()` hand-off, with `run`/`ps`/`kill`/`wait`/`nice` commands calling straight into this subsystem. Verified interactively via QEMU monitor-injected keystrokes against a real spawned/waited/killed `/init.elf`.
7. **Still open from this phase:** per-CPU run queues + spinlocks (§8) — built when SMP bring-up (AP init, per-CPU data) actually lands elsewhere in the kernel, not before; this remains a placeholder marker, not a trigger.

**Phase SMP — real multi-core scheduling (✅ complete; the "SMP shape" Phase D.7 deferred):**

Built and verified under QEMU `-smp 4`, in four sub-phases matching the plan's build→boot→selftest discipline (each landed and re-verified against the full existing suite before the next began):

1. ✅ **Per-CPU foundation** (`kernel/cpu/percpu.h/.c`): `struct cpu_data` per core (own TSS + RSP0/#DF stacks, `current_process`, `pending_reap`, `online`), indexed by an APIC-ID→index table built from the MADT; `this_cpu()` resolves via `lapic_get_id()` (deliberately not GS-base — see the header's comment). `current_process` became `#define current_process (this_cpu()->current_process)` so every existing use site in `process.c` needed zero textual changes. GDT split into `gdt_init_bsp()` (shared descriptors, grown to `5 + 2*MAX_CPUS` entries) + `gdt_init_this_cpu()` (per-core TSS descriptor + `ltr`); `tss_set_rsp0()` now writes `this_cpu()->tss.rsp0` behind the same signature.
2. ✅ **AP bring-up** (`kernel/cpu/smp.c`, `ap_trampoline.asm`, `ap_entry.asm`): INIT-SIPI-SIPI via new LAPIC ICR helpers; a 16-bit real-mode trampoline assembled flat (`nasm -f bin`), embedded in the kernel image via `incbin`, copied to the PMM-reserved page at `AP_TRAMPOLINE_PHYS` (0x8000) and poked per-AP with the kernel PML4 / stack top / `ap_entry64`. APs reuse the kernel's own page tables — made possible by one permanent identity map of `[0, 1MB)` added to the kernel PML4 (`ap_bootstrap_map()`), which stays invisible to ring 3 because `vmm_create_address_space()` zeroes user-half slots. Strictly sequential bring-up gated on each AP's `online` flag, which the AP sets only after **all** of its setup (GDT/IDT/LAPIC/NXE/timer/adopt) is done.
3. ✅ **One global scheduler lock** (`g_sched_lock`), exactly as §8's "single lock first" plan of record: every `thread_table`/`process_table`/`current_thread`/`pending_*_reap` mutation is under it (Phase 4 rename, §6.1), and it is **held across `kernel_ctx_switch`** — released only by whichever context resumes on the far side (the line after the switch, or a brand-new process's trampoline). `running_cpu` on every PCB makes `process_kill()` a three-way branch (own-current / running-elsewhere / not-running-anywhere); the running-elsewhere case marks `ZOMBIE` only and lets the owning core's next tick do the hand-off — no IPI needed. `pmm.c` and `vmm.c` gained their own spinlocks (kheap already had one); `kprintf()` output is serialized by a lock so cross-core prints don't interleave at the byte level; the exception dump takes a never-released `panic_lock` so a second faulting core can't garble the first's report.
4. ✅ **Per-core pinned idle kthreads + liveness invariants**: every core gets a dedicated `PRIORITY_BACKGROUND` idle kthread pinned to it (`pinned_cpu`, skipped by every other core's scan and exempt from aging), plus its adopted bootstrap context (BSP shell / AP `ap_main`), also pinned. This is a **liveness requirement, not tuning**: a core whose current process dies or blocks must always have a legal switch target, or it keeps physically executing on a stack that the woken parent — running concurrently on another core — is about to free. New selftests: `test smp online`, `test smp sched` (8 kthreads must first-execute on ≥ 2 distinct cores), `test smp kill` (kill a kthread live on a different core; assert its counter freezes and its slot is reaped).

**Phase 4 — real process/thread split (✅ complete):**

1. ✅ `struct thread` (schedulable unit) split out of the old conflated `struct process` — see §6.1 for the exact field-by-field breakdown of what moved and why. `thread_table[MAX_THREADS]` (schedulable units) + `process_table[MAX_PROCESSES]` (resource owners) replace the old single `proc_table[MAX_PROCESSES]`.
2. ✅ `current_thread` (real per-CPU field, `cpu_table[]`) + `current_process` (derived, read-only `current_thread->proc` macro) replace the old single per-CPU `current_process` field — designed and shipped so every external consumer outside `process.c` (`cpu/syscall.c`, `cpu/usercopy.c`, `fs/fd.c`) needed either zero changes or a one-line NULL-check fix (`current_process` → `current_thread`, since `current_thread->proc` can't be evaluated when `current_thread` is NULL).
3. ✅ New public API: `struct thread *thread_create(struct process *proc, void (*entry)(void))` — the actual new capability this phase adds (an additional thread sharing an EXISTING process's address space). `process_create()`/`process_create_kthread()` are now thin wrappers: build/find a process, then call the same internal `thread_alloc_for()` helper `thread_create()` uses for the first (and, until now, only) thread.
4. ✅ Two-level deferred reap, per core (§7.4): `this_cpu()->pending_thread_reap` (a thread's kernel stack, freed every exit) and `this_cpu()->pending_process_reap` (a process's address space, freed only when the exiting thread was also the LAST thread of its process). `thread_zombie_locked()` is the new shared decision point between `schedule_locked()`'s self-exit path and `process_kill()`'s external-kill path — decrements `proc->live_thread_count`, decides parent hand-off vs. deferred reap, and is deliberately idempotent against `schedule_locked()`'s pre-existing retry behavior (unlinking the thread from `proc->thread_list` as its first action, so a second call on the same thread is a safe no-op).
5. ✅ `process_kill()` generalized to iterate every thread of the target process (snapshotting the list first, since `thread_zombie_locked()` unlinks as it goes), applying the existing three-way branch (own-current / running-elsewhere / not-running-anywhere) per thread instead of once for the whole process; the caller's own current thread is always processed last, since killing it hands off to `schedule_locked()`, which never returns.
6. ✅ `process_wait()`'s belt-and-suspenders `running_cpu != -1` retry (Bug 23, §16) kept working by mirroring a NEW `running_cpu` field directly on `struct process` (meaningful only once `live_thread_count == 0`), synchronized at the same instant `schedule_locked()` confirms the dying thread's core has actually switched away — see §6.1's field-by-field notes.
7. ✅ Two new selftests, `test thread smp` and `test thread exit` (§12) — prove multiple threads under one process genuinely share its address space (not just its pid), and that the address space survives until the LAST thread exits, not the first.
8. ✅ **Zero new bugs** — build succeeded on the first attempt, and two independent clean boot-to-battery runs (all ten selftests, `-smp 4`) both passed with no failures. Credited to designing explicitly around the two hazards Phase SMP had already taught (idempotent re-entry into exit-disposition logic; a belt-and-suspenders liveness check mirrored across the split) rather than treating this as new, unexamined territory.
9. **Deliberately not built this phase:** any ring-3 `sys_thread_create`/`sys_thread_exit` syscall surface. The plan of record was to land and verify the kernel-internal split first, then decide separately whether userspace needs it — building both in the same pass would have made a regression harder to attribute to one or the other. Built in Phase 5, immediately below, once that verification was in.

**Phase 5 — ring-3 threads (✅ complete):**

1. ✅ `MAX_PROCESSES`/`MAX_THREADS` raised 16/16 → 64/256 (process.h) — 16 was sized for "one process, one thread" plus a handful of kthreads, and stopped being enough headroom once a single ring-3 process could plausibly want more than a couple of real threads. Caught and fixed `KSTACK_SIZE`'s accidental coupling to `MAX_PROCESSES` (`(MAX_PROCESSES * 1024)` happened to equal 16 KiB, but was never actually DERIVED from the process count) in the same pass — raising the ceiling without decoupling this would have silently quadrupled every kernel stack's size.
2. ✅ `struct thread`/`struct process` gained the fields §6.1's Phase 5 addendum describes (`joinable`, per-thread `exit_code`, `user_arg`, `join_wait`).
3. ✅ `thread_create_user(proc, entry_point, arg)` — the ring-3 equivalent of Phase 4's `thread_create()`: an additional thread of `proc`, entering ring 3 via `process_trampoline` (the exact fabricated-context mechanism a process's own first thread already used), with its own dedicated user stack inside the SAME address space (deterministically placed from the thread's own table-slot index — see §6.1's stack-placement note). Marked `joinable`.
4. ✅ `thread_join(proc, tid)` — blocks (a real block via the new `proc->join_wait`, not a busy-poll) until thread `tid` of `proc` exits, then reaps it and returns its own `exit_code`. Mirrors `process_wait()`'s shape (including the same `running_cpu != -1` belt-and-suspenders retry and the same "snapshot/restore the caller's interrupt state across a `schedule_locked()`-only resume" dance, Bug 25 §16) one level down: threads of one process instead of children of one parent.
5. ✅ `thread_exit_self(code)` — the thread-level analog of `process_exit_self()`: ends only the calling thread. Always writes `current_process->exit_code` unconditionally ("last writer wins" across however many threads ever call it) — deliberately no special-casing for "am I the last thread," since `process_exit_self()` never needed that either (there was only ever one thread before this phase).
6. ✅ `schedule_locked()`'s deferred-reap step (§7.4) gained one exception: a joinable thread that is NOT also completing its process is left OUT of the per-core reap queue, so its kernel stack survives for `thread_join()` to collect later — see §7.4's Phase 5 addendum for the exact condition and why a thread that WAS its process's last one doesn't need this (no siblings left to ever join it).
7. ✅ Three new syscalls, `sys_thread_create`/`sys_thread_join`/`sys_thread_exit` (§15.2) — `sys_thread_create` validates its `entry_point` argument with `access_ok()` before ever handing it to the scheduler (an arbitrary ring-3-supplied address, unlike `sys_spawn`'s trusted-loader-chosen ELF entry). Returns a raw `tid` (a `thread_table[]` slot index), not a capability handle like `sys_spawn`'s pid — deliberately: a tid only ever names a thread of the CALLER's OWN process, so there's no cross-process confused-deputy gap the handle indirection exists to close (§15.2's own reasoning for why pids DO need one).
8. ✅ New selftest, `test ring3 threads` (§12) — the first selftest in this file that needs a real disk rather than being purely in-kernel, since it's the only way to exercise the ring-3 half of this feature: `user/init.c` now spawns and joins a second thread of itself, checked via a real shared-`.data` write.
8a. **Test-robustness finding, not a kernel bug** (discovered verifying this phase, §12's `test thread smp` entry): the *existing* `test thread smp` (Phase 4) turned out to have a real, if modest, flaky-failure rate under heavy host CPU contention — a correlated/bursty timing artifact of its own fixed sample size and wait window, not a scheduler correctness issue (the underlying cross-core dispatch was already independently proven correct, including by this exact test passing cleanly, repeatedly, when Phase 4 originally shipped). Mitigated (not eliminated) by raising its sample size 3 → 8 and its wait budget 20 → 100 ticks — recorded here because it's the kind of finding this document's testing discipline exists to catch, even though it didn't touch any code this phase actually shipped.
9. **Known, accepted limitation, documented rather than engineered around:** `tid` is not generation-guarded (no ABA protection) — a stale tid could, in principle, alias a later, unrelated thread that happens to recycle the same `thread_table[]` slot. Unlike `parent_pid` (which guards against exactly this for PIDS, because a raw pid crosses a PROCESS boundary — see §6.2), a tid only ever lets a process misname one of its OWN threads, self-inflicted rather than a cross-process isolation gap, so the same rigor doesn't buy the same safety property here. Same category of trade-off as §7.1's PID-wraparound call: revisit only if this is ever actually hit in practice.
10. **Deliberately not built this phase:** per-thread stack reclamation before process exit (§6.1's Phase 5 addendum — a joined thread's stack pages are reclaimed only when the whole process is, not the instant it's joined); thread-local storage; any signal/cancellation semantics beyond the uncatchable process-level kill (`process_kill()` already forces every thread of a process to `ZOMBIE`, joinable or not — see that function's existing iteration, §13's Phase 4 entry).

**Phase E — `fork()` compat (only if a concrete program needs it):**
1. COW pages in the VMM (explicitly `docs/ARCHITECTURE.md` §3.2's territory, deferred until here).
2. `fork()` as a compat syscall built from `spawn()`'s address-space-construction path, not a parallel implementation.

**Explicitly not scheduled, revisit only on real need:** thread-local storage and tid generation-guarding (Phase 5 notes above), per-CPU run queues (only if `g_sched_lock` is *measured* to bottleneck), TLB shootdown (only when something does a live unmap while other cores may hold the mapping), CFS/EEVDF-style fairness accounting (§4.2), PID reuse/namespace machinery (§7.1).

---

## 14. Dependencies

| Dependency | What this subsystem needs from it |
|---|---|
| `mm/vmm.c` | `vmm_create_address_space`, `vmm_destroy_address_space`, `vmm_switch_address_space` — process address-space lifecycle. COW (Phase E) is a VMM feature this subsystem consumes, not builds. |
| `cpu/kcontext.asm` / `kcontext.h` | The context-switch primitive itself (§4.4, §7.2) — this subsystem must never reach into `struct kcontext`'s fields directly. |
| `cpu/gdt.c` | `tss_set_rsp0` — one TSS per CPU (today: one TSS, period), used only for `RSP0`/`IST`, never hardware task-switching (§4.4). |
| `cpu/elf.c` | ELF loading into a freshly created address space — `process_create`'s dependency, not re-specified here. |
| `cpu/syscall.c` | Where `sys_exit`, future `sys_spawn`/`sys_wait`/`sys_yield` are dispatched from — this subsystem provides the C functions; syscall numbering/ABI is that layer's concern. |
| `cpu/spinlock.c`, `cpu/rwlock.c` | Unused today; become load-bearing at Phase D (§8). |
| `drivers/timer.c` / `cpu/lapic.c` | The timer tick that drives preemption (Phase B) — `lapic_timer_get_ticks()` already exists and is used elsewhere in the kernel main loop today. |

---

## 15. API Design — Kernel & Userspace Interfaces

### 15.1 Kernel-internal API (C functions, this subsystem's actual surface)

```c
/* Lifecycle */
void process_init(void);                          // ✅ built
int  process_create(const char *path);             // ✅ built (spawn()-shaped: builds
                                                     //    the address space directly;
                                                     //    file-actions param still 🎯,
                                                     //    unscheduled — see §17)
                                                     //    Phase 4: internally allocates a
                                                     //    process, then its first thread
                                                     //    via the same helper thread_create()
                                                     //    below uses.
/* process_start_first() -- REMOVED (Phase 4). Zero call sites remained (main.c
 * had already switched to process_adopt_current() below in Phase D); adapting
 * it correctly for the thread/process split (fabricating a thread's ctx,
 * trampoline handling) would have been meaningful unverifiable work for code
 * nothing called, so it was deleted outright rather than carried forward. */
void process_exit_self(int code);                   // ✅ built — sys_exit's real body
                                                     //    (named _self, not process_exit:
                                                     //    it always acts on current_thread,
                                                     //    there is no "exit some other pid")
void process_reap(uint32_t pid);                    // ✅ built — used directly by process_wait
struct process *process_find(uint32_t pid);         // ✅ built — PCB lookup by pid, used by
                                                     //    process_wait and process_kill
struct thread *process_adopt_current(void);         // ✅ built (Phase D; return type updated
                                                     //    Phase 4) — turns the CALLING
                                                     //    execution context into a real,
                                                     //    permanently-scheduled current_thread
                                                     //    (owned by a fresh process sharing the
                                                     //    kernel's own PML4); used by both the
                                                     //    scheduler selftests (paired with a
                                                     //    restore) and main.c's shell
                                                     //    (permanent, no restore call ever)
int process_wait(uint32_t pid);                     // ✅ built (Phase D) — see §7.4. Blocks for
                                                     //    real via child_wait; NOT a busy-poll.
int process_set_priority(uint32_t pid, uint8_t p);  // ✅ built (Phase C; Phase 4: applies to
                                                     //    every thread of the process) — the
                                                     //    `nice` shell command's kernel-side impl
int process_list(struct process_info *out, int max); // ✅ built — `ps`'s kernel-side snapshot;
                                                     //    returns COPIES, never live PCB pointers

/* Scheduling */
void schedule(void);                                // ✅ built (Phase A/B: round-robin,
                                                     //    now timer-preemptible;
                                                     //    Phase C: priority-band-aware, same
                                                     //    signature; Phase 4: scans thread_table)
void sys_yield(void);                               // ✅ built, wired to syscall number 3

/* Blocking (Phase B; signature updated Phase 4) */
void wait_queue_block(struct wait_queue *wq, struct thread *t);  // ✅ built — queues a THREAD
void wait_queue_wake_one(struct wait_queue *wq);                   // ✅ built
void wait_queue_wake_all(struct wait_queue *wq);                   // ✅ built

/* Ring-3 process handles (Phase D, §3.4/§3.5) — unchanged by Phase 4, still process-level */
int  process_handle_alloc(struct process *owner, uint32_t pid);            // ✅ built
int  process_handle_resolve(struct process *owner, int handle, uint32_t *out_pid); // ✅ built
void process_handle_free(struct process *owner, int handle);               // ✅ built

/* Uncatchable kill (Phase B; Phase 4: iterates every thread of the target) */
void process_kill(uint32_t pid);                    // ✅ built — forces ZOMBIE regardless of state

/* Multi-threading, ring 0 (Phase 4) */
struct thread *process_create_kthread(void (*entry)(void), struct process *parent); // ✅ built
                                                     //    (return type updated Phase 4) — a
                                                     //    FRESH process sharing the kernel's PML4
                                                     //    plus its one thread
struct thread *thread_create(struct process *proc, void (*entry)(void)); // ✅ built (Phase 4) —
                                                     //    an ADDITIONAL ring-0 thread under an
                                                     //    EXISTING process; not joinable (auto-
                                                     //    reaped on exit, same as before Phase 5)
struct thread *process_create_idle_for_cpu(uint32_t cpu_index); // ✅ built (Phase SMP; return
                                                     //    type updated Phase 4)

/* Multi-threading, ring 3 (Phase 5) — thread_create()'s userspace-visible
 * equivalent; every thread created this way is JOINABLE (struct
 * thread::joinable), unlike the ring-0 API above. */
int thread_create_user(struct process *proc, uint64_t entry_point, uint64_t arg); // ✅ built —
                                                     //    returns a raw tid (thread_table[]
                                                     //    slot index), or a negative errno
int thread_join(struct process *proc, int tid);     // ✅ built — blocks via proc->join_wait
                                                     //    until thread `tid` exits; reaps it
                                                     //    and returns ITS OWN exit_code
__attribute__((noreturn)) void thread_exit_self(int code); // ✅ built — ends only the calling
                                                     //    thread; process_exit_self()'s analog

/* Scheduler selftests, see §12 */
int process_test_roundrobin(void);                  // ✅ built (Phase B)
int process_test_kill(void);                         // ✅ built (Phase B)
int process_test_reap(void);                         // ✅ built (Phase B)
int process_test_stackguard(void);                    // ✅ built (Phase A)
int process_test_wait(void);                          // ✅ built (Phase D)
int process_test_priority(void);                      // ✅ built (Phase C)
int process_test_smp_sched(void);                     // ✅ built (Phase SMP)
int process_test_smp_kill(void);                      // ✅ built (Phase SMP)
int process_test_thread_smp(void);                    // ✅ built (Phase 4)
int process_test_thread_exit(void);                   // ✅ built (Phase 4)
/* test ring3 threads (§12, Phase 5) has no process_test_* function of its
 * own -- it's a direct process_create()/process_wait() call inlined into
 * selftests.c's dispatch, since it needs a real disk and doesn't fit this
 * file's "temporarily adopt current_thread" selftest convention (§12). */
```

### 15.2 Userspace-facing syscalls (✅ all built)

Per `docs/ARCHITECTURE.md` §3.4's split (fds for streams, typed handles for structural objects): process/thread handles are **not** file descriptors. §3.4/§3.5's capability-handle model for `sys_spawn`/`sys_wait`/`sys_kill` is now built, closing what was previously flagged here as a known divergence (an earlier pass shipped a raw `pid_t`-shaped `uint32_t` first, deliberately, to get a *working* surface before deciding the handle-scoping details). `sys_spawn` allocates a handle in the **caller's own** `struct proc_handle handles[PROC_HANDLE_MAX]` table (process.h) pointing at the real pid; `sys_wait`/`sys_kill` resolve their handle argument back to a pid via that same table before acting — a process can only ever name a pid it was actually handed a handle to, closing the confused-deputy hole a bare pid argument left open (any ring-3 process naming any pid it could guess).

```
sys_exit(code) -> !                          // ✅ built (int 0x80, SYS_exit = 2)
sys_spawn(path) -> handle                     // ✅ built (SYS_spawn = 10) — builds on
                                               //    process_create; returns a handle
                                               //    (process_handle_alloc), not the raw
                                               //    pid. file_actions[] param still 🎯,
                                               //    unscheduled.
sys_wait(handle) -> exit_code                 // ✅ built (SYS_wait = 11) — resolves the
                                               //    handle, then genuinely BLOCKS via
                                               //    process_wait() (§7.4) until that
                                               //    specific child exits or is killed;
                                               //    frees the handle on return either way.
sys_yield() -> void                           // ✅ built (SYS_yield = 3)
sys_getpid() -> pid                           // ✅ built (SYS_getpid = 12) — not in the
                                               //    original spec, added because a spawn()
                                               //    caller needs to tell itself apart from
                                               //    a child running the same binary.
                                               //    Deliberately the real pid, not a
                                               //    handle: a process always has ambient
                                               //    authority over itself.
sys_kill(handle) -> void                      // ✅ built (SYS_kill = 13) — the
                                               //    userspace-reachable edge of the
                                               //    uncatchable kill; process_kill()
                                               //    itself has existed since Phase B
                                               //    (used by the selftests), this just
                                               //    exposes it to ring 3 via the handle
                                               //    table. Does not free the handle --
                                               //    the caller still needs it to
                                               //    sys_wait() and collect the "killed"
                                               //    exit code (-1) afterward.
sys_thread_create(entry, arg) -> tid          // ✅ built (SYS_thread_create = 14, Phase 5) —
                                               //    thin wrapper over thread_create_user()
                                               //    (§15.1), with `entry` validated via
                                               //    access_ok() before ever being handed to
                                               //    the scheduler (see that function's own
                                               //    comment, cpu/syscall.c, for why an
                                               //    arbitrary ring-3-supplied entry point
                                               //    needs this check where an ELF's own
                                               //    entry point, chosen by the trusted
                                               //    loader, doesn't). Returns a raw tid, NOT
                                               //    a handle -- see thread_create_user()'s
                                               //    doc comment (process.h) for why a thread
                                               //    doesn't need the same confused-deputy
                                               //    protection a pid does (no cross-process
                                               //    thread naming exists at all).
sys_thread_join(tid) -> exit_code             // ✅ built (SYS_thread_join = 15, Phase 5) —
                                               //    thin wrapper over thread_join(); a real
                                               //    block via proc->join_wait, same shape as
                                               //    sys_wait/process_wait() one level down.
sys_thread_exit(code) -> !                    // ✅ built (SYS_thread_exit = 16, Phase 5) —
                                               //    ends only the calling thread; if it
                                               //    happens to be the process's last thread,
                                               //    this completes the process too, exactly
                                               //    like sys_exit would.
```

---

## 16. Common Bugs (with concrete examples from this codebase)

This section is written from bugs actually found and fixed, plus bugs found and *not yet* fixed, in this exact tree — because the value of "common bugs" documentation is close to zero when it's generic ("don't corrupt the stack") and high when it's specific enough to recognize the *shape* next time.

**Bug 1 — Wrong selector pushed onto the `iretq` frame (fixed).**
`process_trampoline` builds a ring-3 entry frame with inline assembly:
```c
__asm__ volatile(
    "pushq %0\n"   "pushq %1\n"   "pushq $0x202\n"
    "pushq $2\n"   "pushq %3\n"   "iretq\n"
    : : "r"(ss_sel), "r"(user_rsp), "r"(cs_sel), "r"(entry) : "memory"
);
```
The `"pushq $2\n"` line pushed the **literal immediate 2** instead of `%2` (the register holding the real selector `0x23`). Selector `0x0002` decodes to GDT index 0 — the null descriptor — with RPL 2. Loading `CS` from a null-descriptor selector is illegal and faults immediately: `#GP`, vector 0xD, right on the `iretq`. **The general shape to recognize:** in extended inline `asm`, a template placeholder (`%N`) and a literal immediate (`$N`) are easy to typo into each other, especially when refactoring an early hardcoded value into a real operand — the compiler will not warn, because `%2` being unused as a template reference is not an error (GCC still allocates a register for it; it's simply never emitted). **Guard against recurrence:** for any inline-asm frame construction (there is exactly one other place this pattern could appear — nowhere else in this kernel yet, but syscall entry/exit paths are the next candidate), diff the operand list against every `%N`/`$N` in the template by hand; this class of bug is invisible to the compiler by construction.

**Bug 2 — Struct field too narrow for the value it stores (fixed).**
`struct process::kstack_top` was declared `uint16_t`, but `alloc_kernel_stack()` returns a full 64-bit kernel-heap virtual address (something in the `0xFFFFFF80'00000000`-range direct map). The assignment `proc->kstack_top = kstack_top;` silently truncated to the low 16 bits — valid C, no warning by default (no `-Wconversion` in this project's build flags per `docs/ARCHITECTURE.md` §8's conventions). The bug hadn't fired yet only because Bug 1 crashed first; it would have handed `tss_set_rsp0()` a garbage address the moment any ring-3 process took an interrupt. **The general shape to recognize:** a field's declared width silently disagreeing with what's actually stored in it is invisible until the high bits happen to matter — which for a kernel-heap pointer is *always*, just not on the first read if the low bits happen to look plausible. **Guard against recurrence:** any struct field holding a pointer or VA must be `uint64_t` (or the appropriate pointer type) — never a "seemed small enough" integer type chosen without checking what's actually assigned to it.

**Bug 3 — Zombie processes are never reclaimed (fixed).**
`sys_exit` sets `PROCESS_ZOMBIE` and calls `schedule()`; nothing transitioned a `ZOMBIE` PCB back to `UNUSED`, freed its kernel stack, or destroyed its address space. With `MAX_PROCESSES = 16`, this was a hard ceiling: the sixteenth `process_create` after boot would fail forever, silently (returns `-EMBK_ENOMEM`, indistinguishable from genuine memory exhaustion) — a debugging trap for whoever hit it first, since the symptom ("process creation fails") gives no hint that the actual cause is "the table is full of corpses." **The general shape to recognize:** a state machine (§7) with a terminal state that has a documented exit transition (`ZOMBIE → UNUSED` is drawn in every version of this diagram from the start) but no code implementing it — the diagram existing is not evidence the transition is wired up. **Fix:** `process_reap_slot`, deferred one `schedule()` call behind the actual exit (see §7.4 for exactly why it can't run immediately). **Still open:** this has not actually been observed reclaiming anything — `main.c` only ever creates one process today, so `g_pending_reap` never gets set (there's nothing else to switch to, `schedule()` returns before reaching that code). The fix is correct by construction and code review, not yet by observation.

**Bug 4 — No kernel stack guard page (fixed).**
`alloc_kernel_stack()` was a bare `kmalloc(KSTACK_SIZE)` — 16 KiB, adjacent in the heap to whatever the allocator placed next to it, with no unmapped guard page below the stack's low address. A kernel-mode stack overflow (deep recursion, a large stack-allocated local — both realistic in an ELF loader or a filesystem walk) would have silently corrupted the adjacent heap object instead of faulting. **The general shape to recognize:** "it hasn't crashed yet" is not evidence a stack is deep enough — it's evidence nothing has *yet* recursed deeply enough to prove it isn't, which is precisely why this was listed as a **Failure Mode** (§10) rather than something to wait for a real report on. **Fix:** `vmm_alloc_kernel_stack`/`vmm_free_kernel_stack`, page-mapped with a genuinely unmapped guard page directly below (see §7.2's dependency on the VMM's kernel-mapping strategy — this is exactly the kind of change that section warned to check against).

**Bug 5 — The Bug 4 fix itself broke the first process's own page tables (found and fixed in the same session).**
The natural way to give the new kernel-stack region its own address space is a fresh, previously-untouched virtual-address range — that's what was tried first (a dedicated base, unrelated to any existing region). It's wrong, for a reason specific to how this kernel shares address spaces: `vmm_create_address_space()` builds a new process's PML4 by **copying the kernel-half entries by value, once, at that exact moment** (`pml4[i] = kernel_pml4[i]` for the shared indices) — not by pointing at a live, shared table. A PML4 slot that's still not-present at the moment of copying stays not-present in that process forever, even after the kernel's own table fills it in moments later. Since `vmm_alloc_kernel_stack()` populates the new region's slot **for the first time inside `process_create()`, after that same process's address-space snapshot was already taken**, the process's own page tables never saw its own stack mapping. Result: `#PF` on the very first stack access, which cascades to `#DF` because the fault handler can't even push its own frame — `TSS.RSP0` points into the same unmapped region. Caught immediately (not from a report weeks later) because the fix was verified against a *real* run of `/init.elf` through EMBKFS, not just a clean build and a smoke-test boot with no filesystem attached — the boot-only test genuinely could not have caught this, since `process_create` never reached the new code path when ELF loading failed first. **Fix:** put the kernel-stack region inside `MMIO_BASE`'s existing PML4 slot instead (256 GiB in, far from where the real MMIO bump allocator grows) — that slot is guaranteed already-populated because HPET/LAPIC/IO-APIC/the framebuffer all call `vmm_map_mmio()` during early boot, before any process exists. **The general shape to recognize:** any new "shared across every address space" region must be populated (at least its top-level page-table entry) *before* the first `vmm_create_address_space()` call, or it must reuse a PML4 slot that's already guaranteed populated by then — "it's in the kernel's own page tables" is not sufficient if the sharing mechanism is a point-in-time copy rather than a live reference. **Guard against recurrence:** before adding another region sharing this pattern, check §7.2 and pick an already-populated slot (or move the reservation to `vmm_init()`, before any process can possibly exist) rather than asserting a fresh one is safe.

**Bug 6 — RFLAGS.IF corruption in the context-switch primitive, exposed the moment real preemption existed (found and fixed the same session Phase B landed).**
`kernel_ctx_switch`/`kernel_ctx_save` (`kcontext.asm`) capture the outgoing process's flags with `pushfq`/`pop rax` and store them verbatim into the saved `struct kcontext`. This is correct *only* if the live flags at that instruction genuinely reflect the outgoing process's state — which silently stopped being true the moment `schedule()` started being called from inside an interrupt-gate ISR (the LAPIC timer handler, Phase B's whole point). Interrupt gates auto-clear IF on entry by CPU design (that's what distinguishes them from trap gates) — so *every* context saved from inside the timer ISR (or from `int 0x80`, also a gate) recorded IF=0, regardless of whether the process being switched away from actually had interrupts enabled. Symptom: `test sched roundrobin` hung after exactly one preemption cycle — the first process to be preempted and later resumed came back with IF=0 permanently, so its next `hlt` (or any wait for a future timer tick) never woke. **The general shape to recognize:** any code that reads *live* CPU/architectural state (flags, in this case) from inside a handler that itself alters that state on entry is reading the handler's state, not the interrupted context's — the fact that it "looks like a normal read" (`pushfq`; nothing exotic) hides that the value is already wrong before the instruction even runs. **Fix:** `or rax, 0x200` (force IF=1) before storing, in both `kernel_ctx_switch` and `kernel_ctx_save` — justified because reaching that save point at all is itself proof IF was 1 moments earlier (a maskable IRQ literally cannot be taken with IF=0, and ring 3 can't execute `cli`/`sti` — privileged, `#GP`), so forcing it back to 1 is restoring the true prior state, not guessing. **Guard against recurrence:** any future field added to `struct kcontext` that's captured via a `push`-and-read-back instruction (not an explicit software-maintained variable) needs the same audit — "is this instruction's *live* result equal to the thing I actually want to snapshot, given where I'm calling it from?"

**Bug 7 — `int 0x80` leaves IF=0 for the entire syscall body (found while verifying file I/O syscalls, same root cause as Bug 6, different symptom).**
`int 0x80` is implemented as a software interrupt gate (not a trap gate), so — exactly as in Bug 6 — IF is cleared on entry and stays cleared for the whole handler unless something explicitly re-enables it. `sys_open`'s call into `vfs_open`'s disk read blocks waiting on the storage controller's completion IRQ; with IF=0 for the syscall's entire duration, that IRQ could never be delivered, so the syscall hung forever the first time any syscall did real I/O (write-to-serial doesn't block, so this had never fired before file I/O syscalls existed). **The general shape to recognize:** "interrupt gate" is not just an implementation detail of how the vector is dispatched — it changes what's *legal to do* inside the handler, and "wait for a different interrupt to satisfy this one" is exactly the kind of thing that looks fine in isolation (it's just a wait loop) but is only safe under a trap gate or with an explicit `sti`. **Fix:** `sti` as literally the first instruction of `syscall_dispatch()` — deliberately unconditional and unconditionally safe, because every syscall body either doesn't touch interrupts at all or actively needs them on (there's no syscall in this kernel that needs IF=0 for its own duration). **Guard against recurrence:** any new interrupt-gate handler that might call into code performing blocking I/O (waiting on a *different* IRQ) needs this same audit before it ships, not after the first hang report.

**Bug 8 — `proc_alloc()` marked a brand-new PCB schedulable before it was actually built, exposed only once real preemption and a second live `process_create()` call could coexist (found while verifying `sys_spawn`).**
`proc_alloc()` reserved a free slot and immediately set `state = PROCESS_READY` — harmless for the entire time only one process ever existed (Phase A), because nothing else could possibly get scheduled in between reservation and the rest of `process_create()` finishing. The instant Bugs 6/7 were fixed and preemption + `sys_spawn` both existed, this became live: a `sys_spawn` call running as a syscall could itself be preempted mid-`process_create()` — specifically during the slow ELF-load disk I/O — and because the new slot was already `READY`, the scheduler was free to pick it. But at that point the new PCB had no valid `pml4_phys`, no `kstack_top`, no initialized `ctx` — dispatching to it crashed. **The general shape to recognize:** a two-phase "reserve, then build" allocator pattern is only safe if nothing outside the allocator can observe or act on the reserved-but-unbuilt object — true by accident under cooperative-only scheduling, false the instant preemption exists, and the two phases are far enough apart in the source (`proc_alloc()` vs. the end of `process_create()`) that the hazard isn't visible by reading either function alone. **Fix:** `proc_alloc()` now marks the slot `PROCESS_BLOCKED` (excluded from scheduling, and not confused with a real wait-queue block per the invariant-4 note in §7) instead of `READY`; `process_create()`'s own final line is the sole place that transitions to `READY`, after `pml4_phys`/`kstack_top`/`ctx` are all genuinely valid.

**Bug 9 — PCB slot permanently leaked on every `process_create()` error path (found via code review while fixing Bug 8, same session).**
While auditing `process_create()`'s failure paths to fix Bug 8, none of the four early returns (address-space creation failure, ELF load failure, user-stack allocation failure, kernel-stack allocation failure) reset `proc->state` back to `PROCESS_UNUSED` before returning an error — each one left the slot in whatever partially-reserved state `proc_alloc()` had put it in, permanently unusable and indistinguishable (from outside) from a real running process's slot. With `MAX_PROCESSES = 16`, repeated failed spawns (e.g., a bad path passed to `sys_spawn`) would silently shrink the usable process table over the kernel's uptime, eventually reproducing exactly Bug 3's original symptom (`process_create` fails forever) but from a completely different cause. **The general shape to recognize:** every early-return failure path in a multi-step "acquire, initialize, initialize, initialize" constructor needs to undo *all* prior acquisitions, not just the specific resource that failed on this attempt — a constructor audited only for its happy path is a leak audited for zero of its paths. **Fix:** added `proc->state = PROCESS_UNUSED;` to all four early-return sites. **Guard against recurrence:** any future step added to `process_create()` needs a corresponding rollback added to every return path that comes *after* it, not just the one immediately following.

**Bug 10 — `schedule()` was reentrant against the timer ISR whenever called from syscall context (found by re-reading the finished Phase B work, not by a crash — fixed same session).**
Bugs 6 and 7 each independently established that IF can be 1 during a `schedule()` call: Bug 6 because the timer ISR's *saved* snapshot forces IF=1 for the outgoing process, and Bug 7 because `syscall_dispatch()` does an unconditional `sti` before dispatching to `sys_exit`/`sys_yield`, both of which call `schedule()` downstream (`process_exit_self` → `schedule()`, `sys_yield` → `schedule()`). Put those two facts together and a third one falls out that neither bug's fix addressed: a `schedule()` call reached from syscall context runs with *live* IF=1 for its own entire body — the scan of `proc_table`, the `g_pending_reap` reclaim, the `current_process`/state mutations, and the CR3/RSP0 switch all execute with interrupts enabled. A timer IRQ landing anywhere in that window calls `schedule()` again, reentrantly, on the same kernel stack, while the outer call's local variables (`prev`, `next`) and the global `current_process`/`proc_table` state are mid-mutation. Depending on exactly where the two calls interleave, this can range from redundant-but-harmless (interrupt lands before any state is touched) to genuinely corrupting (interrupt lands after `current_process = next` but before `kernel_ctx_switch`, so the inner call captures a `prev` that the outer call is about to save over, or the inner call fully completes a switch via its own `kernel_ctx_switch`, which never returns to the outer call's remaining lines until this process is independently rescheduled far later — at which point the outer call resumes using state that a completely different scheduling decision has since invalidated). **The general shape to recognize: fixing two separate bugs that each individually justify "IF can be 1 here now" does not automatically mean the code in between was audited for what "IF can be 1 here" actually implies** — Bugs 6 and 7 were each scoped to their own specific symptom (a resumed process stuck with interrupts off; a syscall hung on disk I/O), and neither fix's author needed to reconsider `schedule()`'s own reentrancy to close their own bug. The reentrancy hazard was a side effect of the *combination*, sitting one level up from either individual fix, and nothing forced it to be revisited until this document was being written and the two facts were laid out next to each other in §8. No test in the existing suite exercises the exact interleaving needed to trigger it, which is precisely why it went unnoticed rather than unnoticeable. **Fix:** `schedule()` now disables interrupts at entry and restores the caller's original state on every return path that doesn't go through `kernel_ctx_switch` (full detail and the reasoning for why the post-switch path needs no explicit restore: §8(a)) — the same `pushfq`/`cli`/conditional-`sti` idiom `cpu/spinlock.c`'s `spin_lock`/`spin_unlock` already use elsewhere in this kernel, applied here for the first time. Verified by rerunning `test sched roundrobin`/`kill`/`reap`/`stackguard` in QEMU after the change (all still pass) — `roundrobin` and `kill` in particular already have kthreads calling `process_exit_self` (the same call shape as `sys_exit`) while the timer actively preempts them, which is the closest existing coverage of this exact interleaving, even though nothing in the test asserts on the reentrancy question directly. **Guard against recurrence:** whenever two bugs each conclude "X can happen now, and that's fine because Y" about the same shared piece of state or code, check whether X-and-Y-together implies something neither bug individually needed to consider — the combination is where this one hid.

**Bug 11 — `schedule()`'s zombie hand-off ran after the "nothing else runnable" early return, deadlocking the very first real use of `process_wait()` (found immediately by `test sched wait` hanging, fixed same session).**
`schedule()`'s structure was: find `next` (the process to switch to); if none exists (`!next || next == current_process`), restore interrupt state and return early; only *after* that check, if a switch is actually happening, decide what to do with a `PROCESS_ZOMBIE` `prev` — hand it off to a live parent's `zombie_head`/`child_wait`, or queue it for auto-reap via `g_pending_reap`. This was correct for every scenario that existed before `process_wait()`: `process_test_reap()`'s kthreads always exit while their test harness (`self`) is still `READY` or `RUNNING` (it's mid-`selftest_wait_ticks`, looping `hlt`), so `next` always finds `self` regardless of ordering, and the code after the early return always runs. `process_wait()` creates a genuinely new shape: a parent that's actively `PROCESS_BLOCKED` (via `wait_queue_block`) specifically because it's waiting for this exact child to exit. `PROCESS_BLOCKED` doesn't match the `next`-search's `READY || RUNNING` filter — so when the child (the only *other* process in the table) becomes a zombie and calls `schedule()`, the search finds nothing (`next == NULL`), the function returns early, and the code that would move the child onto the parent's `zombie_head` and call `wait_queue_wake_one()` to flip the parent back to `READY` never runs. The parent stays `BLOCKED` forever; the child stays an unreclaimed zombie forever; nothing in the system will ever call `schedule()` again on their behalf (no other process exists to generate a timer-tick opportunity for *this* pair specifically, though in the actual selftest the whole system just hung on this one blocked call). **The general shape to recognize:** an early-return guard ("nothing else to do, bail out") that was written when the *only* two things that could happen next were "keep running yourself" or "run a different runnable process" silently stops being exhaustive the moment a *third* state (`BLOCKED`, waiting specifically on the thing about to change) enters the picture — the guard's condition (`!next`) doesn't know that the side effect it's skipping past is the very thing that would have produced a `next`. **Fix:** moved the zombie hand-off/auto-reap decision to run unconditionally as soon as `current_process->state == PROCESS_ZOMBIE` is observed, *before* the `next`-search begins — so a wake-up that makes the parent `READY` happens in time for that same `schedule()` call's search to find it. The `RUNNING → READY` demotion for a *non-zombie* `prev` correctly stays exactly where it was (gated behind an actual switch happening), since a still-alive process legitimately might end up "resuming as itself" if it's the only one left — the two cases needed different treatment precisely because only one of them ever needs to survive being skipped. **Guard against recurrence:** any future process state that's excluded from the scheduler's "is there something else to run" search (i.e., anything beyond `READY`/`RUNNING`) needs an audit of every place that transitions a process *out* of that excluded state, to check whether the transition's own side effects need to happen before or after the search that state is invisible to.

**SMP bug ledger (Bugs 12–25) — every one found by an actual crash, hang, or GDB session while landing the SMP phase (§13), condensed because the full essay treatment above would triple this file.** The unifying lesson: almost none of these were "add a lock" bugs — they were *liveness and ordering* bugs in the seams between "this core" and "any core", each invisible on a single core by construction.

12. **AP marked `online` before its setup finished** — `smp_bringup()` starts the next AP the moment the flag flips, so an early flip meant two APs running unserialized setup concurrently (shared IDT writes, unlocked `kprintf`) → garbled serial + #DF. Flag now set as `ap_main()`'s last act.
13. **Scheduler scan matched *any* `PROCESS_RUNNING` candidate** — on one core, "RUNNING" could only mean "me", so the old scan's `READY || RUNNING` filter was equivalent to "READY or myself". With N cores it silently meant "READY, or *whatever someone else is executing right now*" → `kernel_ctx_switch` into a context another core was live inside (RSP=0 #DF). Now `READY || (candidate == current_process && RUNNING)`.
14. **…and the first fix of #13 dropped the state check entirely**, letting a ZOMBIE `current_process` re-select *itself* through the self-fallback and resume past `process_exit_self()`. Both clauses are needed; the comment at the scan spells out why.
15. **`g_sched_lock` was never released on a brand-new process's first dispatch** — a fabricated `ctx.rip` jumps straight to the entry point, skipping the `spin_unlock` that lives right after `kernel_ctx_switch`. Every core eventually parked in `spin_lock` forever. Fix: `process_trampoline()`/`kthread_trampoline()` release the lock as their first action, and `process_create*()` fabricate `rflags` with IF=**0** so interrupts cannot fire in the gap between the switch's `popfq` and that unlock (a real, observed self-deadlock window, not a nicety).
16. **`EFER.NXE` is a per-core MSR** — only `vmm_init()` (BSP) ever set it, so every `VMM_NX` PTE (every kernel stack) was a *reserved-bit* #PF on any AP → #DF the first time an AP dispatched a kthread. `vmm_enable_nx_this_cpu()` now runs first thing in `ap_main()`.
17. **`vmm.c` had zero locking** — kthread creation maps stack pages with no scheduler lock held while a reap on another core unmaps under `g_sched_lock`: two different locks (one nonexistent) around the same page-table pages. `vmm_lock` now guards map/unmap/create/destroy + both VA bump allocators, same shape as `pmm_lock`/`heap_lock`.
18. **`process_exit_self()`'s `cli; hlt` fallback stranded cores** — "nothing else runnable *on this core right now*" stopped implying "the system is done" the moment other cores could be busy running everything else; parking with interrupts off made it permanent (and killed the keyboard when it was the BSP). Now `sti; hlt` so the core's own next tick retries.
19. **Deferred reap posted before the switch away was certain** — `pending_reap` was set in the ZOMBIE hand-off block, but if the scan then found nothing to switch to, the core kept idling *on the zombie's stack* and its own next tick freed that stack out from under itself (#DF inside `vmm_flush_tlb`). The assignment now happens only after a real switch is committed.
20. **`kernel_ctx_switch` force-restoring IF=1 for *resumed* contexts** — every resume lands one `jmp` before `schedule_locked()`'s own `spin_unlock`; forcing IF=1 in the *saved snapshot* (Bug 6's fix, correct then) let a tick land in that gap and re-enter `schedule()` on a lock the core still held. The switch now saves flags verbatim (IF=0, since every save happens inside the timer ISR); `spin_unlock` is what turns interrupts back on. Bug 6's original concern is still honored — by the unlock's saved-flags restore, not by the snapshot.
21. **…and the complementary kthread half:** `spin_unlock`'s conditional `sti` restores the *lock acquirer's* interrupt state — IF=0 when the dispatch came from the timer ISR — and a kthread never passes through an `iretq` that would fix it up, so it ran with interrupts permanently off (unpreemptable forever). `kthread_trampoline()` executes an explicit `sti`; `process_trampoline()` never needed it (its `iretq` pushes `rflags=0x202`).
22. **`process_init()` ran *after* `smp_bringup()`** — the single most damaging one: the blanket table reset wiped the PCBs the APs had already adopted, leaving every AP's `current_process` dangling at a slot the allocator happily recycled into test kthreads — two cores executing "the same" PCB, the root behind a whole family of intermittent corruption that kept shifting shape as the bugs above were fixed. Ordering in `main.c` is now explicit and commented as load-bearing.
23. **A woken parent could reap a zombie whose core was still standing on its stack** — the hand-off (link + wake) is posted *before* the dying core has switched away; if the parent won the race on another core, `process_reap_slot()` freed a stack that was still someone's live RSP. Closed structurally: the dying core clears `running_cpu` and completes its switch **inside the same `g_sched_lock` hold** that posted the hand-off (so the parent can't even enter its locked zombie-walk until the core is off the stack), the per-core pinned idles guarantee that switch always has a target, and `process_wait()` keeps a belt-and-suspenders `running_cpu != -1` busy-retry.
24. **Idempotence of re-entered exit paths** — a ZOMBIE (or BLOCKED) current that couldn't switch away re-runs its disposition logic on every subsequent tick; the hand-off re-link and `wait_queue_block()` re-insert each made the node its own list successor (infinite walk for whoever traverses next). Both are now guarded (`already on the parent's zombie list?` / `already on this wait queue?`).

25. **A resumed voluntary-block caller has NO iretq to restore its interrupt state** — the direct consequence of Bug 20's verbatim-flags fix, one layer up (the exact "two fixes compose into a third bug" shape Bug 10 warned about): a preempted process gets IF back from its timer ISR's `iretq`, a syscall from its `int 0x80` frame's — but `process_wait()`'s blocking `schedule_locked()` call is a plain function call, so after resume the flags are the switch's verbatim IF=0 followed by `spin_unlock` restoring the *dispatching* core's (also IF=0, mid-ISR) state. The shell returned from every blocking wait with interrupts silently off; the next `hlt` downstream hung the core — intermittent only because the blocking path itself is (often the zombie is already handed off and no switch happens). Fix: `process_wait()` snapshots its caller's IF at entry and restores it after every resume. Any future voluntary-block path (a sleep syscall, a mutex) needs the same, and this is now the documented rule.

Also fixed in passing, as a *test* bug rather than a kernel bug: `test sched kill` asserted the victim's slot was `UNUSED` synchronously after `process_kill()` — true single-core behavior, wrong under SMP where a victim running on another core is (correctly) only marked ZOMBIE and reaped by that core's own next tick. The test now polls briefly for the slot to clear.

---

## 17. Trade-offs (explicit, not buried in the prose above)

| Decision | What we gave up | Why it's still right, for now |
|---|---|---|
| Round-robin, not priority/fair-share (§4.2 Phase A/B) | Interactive responsiveness under contention | Real preemption now exists (Phase B), so contention is no longer hypothetical, but `MAX_PROCESSES=16` on a single-user workstation still doesn't produce enough runnable processes at once for fairness to matter in practice; building fairness accounting now is still solving next year's problem with this year's guesses — revisit if it's ever actually observed to matter |
| ~~`sys_wait` busy-polls instead of blocking~~ — ✅ resolved (Phase D) | *(historical)* CPU cycles spent spinning/yielding while a parent waited, instead of the caller being genuinely off the run queue | Was a deliberate, correct stand-in shipped first (busy-polling doesn't need parent/child tracking); `process::parent`/`parent_pid`/`zombie_head`/`child_wait` (§6.2) landed shortly after and `process_wait()`/`sys_wait` now genuinely block — see §7.4. Row kept here as the record of the trade-off actually having been paid off, not forgotten. |
| ~~`sys_spawn`/`sys_wait` took a raw `uint32_t` pid~~ — ✅ resolved (Phase D) | *(historical)* The confused-deputy protection `docs/ARCHITECTURE.md` §3.4/§3.5's handle model was designed to provide (a raw pid let any caller name any other process) | Was the pragmatic choice to ship a coherent, testable spawn/wait unit before designing handle-scoping; the capability-handle model (§15.2) landed shortly after — `sys_spawn` now returns a handle, `sys_wait`/`sys_kill` take one, translated per-process via `handles[PROC_HANDLE_MAX]`. Row kept as the record of the trade-off having been paid off. |
| ~~Process/thread kept unified for now~~ — ✅ resolved (Phase 4, §4.1/§6.1/§13) | *(historical)* The "correct" separation from day one | Was the pragmatic choice to defer real work until something actually needed >1 thread per process; the split (`struct thread` + `struct process`, §6.1) landed once `thread_create()` became the concrete need, cleanly (zero new bugs, §13's Phase 4 entry) — largely because deferring it meant the split was designed against the SMP phase's already-learned hazards (idempotent re-entry, belt-and-suspenders liveness checks) instead of being new, unexamined territory. Row kept as the record of the trade-off having been paid off, not forgotten. |
| ~~Ring-3 thread syscalls not built alongside the kernel-internal split~~ — ✅ resolved (Phase 5, §13) | *(historical)* A complete userspace-facing multi-threading story in the same pass as Phase 4 | Was the pragmatic choice to land and verify the kernel-internal split first (ring 0 only, via `thread_create()`), so a regression was unambiguously attributable to one layer or the other; `thread_create_user()`/`thread_join()`/`sys_thread_create`/`sys_thread_join`/`sys_thread_exit` (§13's Phase 5 entry) landed once that verification was in. Row kept as the record of the trade-off having been paid off. |
| Ring-3 thread stacks reclaimed only on process exit, not per-thread on join (Phase 5, §6.1) | Prompt memory reclamation for a process that creates and joins many short-lived threads over a long lifetime | `thread_join()` already has to reap the THREAD (kernel stack) — extending that to also unmap the joined thread's USER stack pages is real extra work (a `vmm_unmap_in()`-shaped primitive doesn't exist yet) for a cost that's bounded and self-correcting (the whole address space, stacks included, is reclaimed the moment the process itself exits) — not worth building until a real workload's actually observed to need it |
| Thread tids (Phase 5) are not generation-guarded (no ABA protection) | The same slot-recycling safety `parent_pid` already provides for pids (§6.2) | A tid only ever lets a process misname one of ITS OWN threads (no cross-process thread naming exists at all, unlike a pid) — self-inflicted, not the cross-process isolation gap `parent_pid` closes, so the same rigor doesn't buy the same safety property; revisit only if this is ever actually hit in practice, same call as §7.1's PID-wraparound trade-off |
| Static `MAX_PROCESSES`/`MAX_THREADS` arrays, not a dynamic allocator (§6.3) | Scaling past a fixed number of concurrent processes/threads (16/16 through Phase 4; raised to 64/256 in Phase 5 once ring-3 processes could genuinely want more than one thread each) | A dynamic PCB/TCB allocator is real work (allocation failure paths, iteration without a fixed bound) that a single-user dev workstation doesn't need yet; revisit if the new ceiling is ever actually hit in practice, not before |
| No fairness/anti-starvation until Phase C (§4.2) | Guaranteed forward progress for low-priority work under Phase C priorities | Aging (§6.2) is the cheap fix, deliberately deferred to the same phase that introduces the problem it solves — no reason to build it before priorities exist |
| Uncatchable kill bypasses the message-port model entirely (§9) | Architectural purity ("everything goes through ports") | A purely cooperative kill mechanism is provably incomplete (a process that ignores its port can't be stopped by definition) — this is not a purity/pragmatism trade-off, it's a correctness requirement, which is why `docs/ARCHITECTURE.md` §3.3 already calls it non-optional |
| `fork()`/COW pushed to Phase E, off the near-term path | Whatever software assumes `fork()` semantics (most Unix software) | `docs/ARCHITECTURE.md` §3.2 already made this call — repeated here only to note the scheduler-side consequence: no process-duplication code path needs to exist until Phase E, which is why `process_create` today builds one address space from scratch and nothing about its shape needs to anticipate duplication |

---

## 18. References

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A — §7 (Task Management; note §7.2.1 on hardware task-switching's restricted role in 64-bit mode, relevant to §4.4), §6 (Interrupt/Exception handling, relevant to preemption in Phase B).
- Baumann, A. et al., *"A fork() in the road"*, HotOS 2019 — already cited in `docs/ARCHITECTURE.md` §3.2; the `spawn()`-vs-`fork()` asymmetry argument this spec's §4.1/§17 build on.
- Windows scheduling: *Windows Internals* (Russinovich, Solomon, Ionescu), chapter on Processes, Threads, and Jobs — the priority-boost/interactivity lineage cited in §4.2.
- FreeBSD ULE scheduler: Roman Divacky / Jeff Roberson's original ULE design notes — the per-CPU multi-queue lineage cited in §4.2, §8.
- XNU: Amit Singh, *Mac OS X Internals* — the `proc`/`task`/`thread` three-layer split cited in §4.1.
- `docs/ARCHITECTURE.md` §3.1–3.5 — the settled project-wide decisions this entire spec is downstream of.
- `docs/GDB_CHEATSHEET.md` — the debugging workflow referenced in §11.
