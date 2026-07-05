# EmbLinkOS — Process & Scheduling Architecture

*Subsystem specification. Companion to `docs/ARCHITECTURE.md` §2 (Stack), §3.2–3.3 (settled decisions this spec builds on), §5 (roadmap item 3). Written against the kernel as of 2026-07-05 (commit range through the process-init bring-up on `Teo`).*

*Update, same day: Phase A of the roadmap (§13) landed — zombie reclamation and the kernel-stack guard page (Bugs 3 and 4 below) are now fixed, and a fifth bug (PML4-sharing) was found and fixed while verifying the guard-page fix end to end. Sections below are updated in place; nothing here was re-derived, only corrected against what actually shipped.*

*Update, later the same day: Phase B landed in full — wait queues, timer-driven preemption, the uncatchable kill, per-process fd tables, and `sys_wait`/`sys_spawn`/`sys_yield`/`sys_getpid`. Four more bugs were found and fixed while verifying Phase B end to end (Bugs 6–9, §16), all specific to the interaction between real preemption and code that had only ever run with a single process. Sections below are again updated in place against what actually shipped, not re-derived.*

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
| Process table | Static array, `MAX_PROCESSES = 16` (`process.h`) |
| Scheduling policy | Strict round-robin, no priority, no fairness accounting |
| Preemption | ✅ Built. `lapic_timer_handler` calls `schedule()` on every tick (100Hz) after `lapic_send_eoi()` — genuine timer-driven round-robin, not just syscall-triggered. See Bugs 6 and 7, §16, both found while verifying this. |
| Blocking | ✅ Built. `struct wait_queue` + intrusive `process::wait_next`; `wait_queue_block`/`wait_queue_wake_one`/`wait_queue_wake_all` implement §7.3 as specified. |
| Termination reclamation | ✅ Fixed and now exercised. `process_reap()` frees the PCB/kernel stack/address space, deferred one `schedule()` call behind the actual exit (see §7.4). Previously unexercised in practice; now proven live by `test sched reap` and by real multi-process use (`sys_spawn`/`sys_wait`). See Bug 3, §16. |
| Uncatchable kill | ✅ Built. `process_kill(pid)` forces `PROCESS_ZOMBIE` regardless of current state (including `BLOCKED`, unlinking from whatever wait queue it's on), reaps immediately unless it's the caller itself. Verified via `test sched kill`. |
| Per-process file descriptors | ✅ Built. `struct process` now embeds `struct fd_entry fds[FD_MAX_OPEN]`; `fs/fd.c`'s `fd_table()` helper returns it (or a boot-time-only global table before any process exists). Unblocks `spawn()`'s file-action model (§3.2). |
| Kernel stack guard page | ✅ Fixed. `vmm_alloc_kernel_stack`/`vmm_free_kernel_stack` page-map the stack with an unmapped guard page directly below it, replacing the flat `kmalloc`. See Bug 4, §16 — and Bug 5, found while verifying this fix. |
| Syscalls reaching this subsystem | ✅ Built. `sys_exit`, `sys_yield`, `sys_spawn`, `sys_wait`, `sys_getpid` all wired to syscall numbers and dispatched from `syscall_dispatch`. |
| Automated selftests | ✅ Built. `test sched roundrobin`/`kill`/`reap`/`stackguard` (`process_test_*` in `process.c`), matching §12's four specified tests exactly. |
| SMP | Single core. `current_process` is one global pointer, not per-CPU. Unchanged by Phase B — flagged again in §8, now with a real reentrancy hazard (preemption) instead of a hypothetical one. |

Nine concrete bugs have now been found (and all nine fixed) in this subsystem, documented in full in §16, Common Bugs, because they are the kind of bug this class of code produces repeatedly — worth remembering the *shape* of the mistake, not just the fix. The first five were found bringing up a single process (Phase A); the last four were found bringing up real preemption, multi-process syscalls, and per-process fd tables (Phase B) — every one of them a case of code that was correct for exactly one process silently breaking the instant a second process and real preemption coexisted:

1. **The ring-3 entry trampoline pushed the wrong CS selector** (a literal immediate instead of the intended register operand) — corrupted `iretq` frame, `#GP` on every process start.
2. **`kstack_top` was declared `uint16_t`** in a struct that stores a 64-bit heap address in it — silent truncation that would have corrupted `TSS.RSP0` the first time a ring-3 process took any interrupt.
3. **Zombie processes were never reclaimed** — closed a hard ceiling of exactly `MAX_PROCESSES` process creations ever.
4. **No kernel-stack guard page** — a stack overflow silently corrupted adjacent heap memory instead of faulting.
5. **Fixing #4 introduced a new bug**: the replacement kernel-stack VA region landed in a PML4 slot never touched before boot, invisible in the first process's own page tables (`vmm_create_address_space()` shares the kernel half by copying PML4 entries *by value* at creation time, not live) — `#PF` cascading to `#DF` on the very first process. Caught immediately by actually running `/init.elf` end to end rather than trusting the build + a no-filesystem boot smoke test.
6. **RFLAGS.IF corruption across a context switch** — `kernel_ctx_switch`/`kernel_ctx_save` capture RFLAGS with `pushfq` from inside an interrupt-gate ISR, where IF is always 0 by construction, so every saved snapshot claimed IF=0 regardless of the outgoing process's real state; the first resumed process therefore came back with interrupts permanently masked.
7. **`int 0x80` leaves IF=0 for the entire syscall** — same underlying fact as #6 (interrupt gates auto-clear IF), different symptom: any syscall blocking on a hardware completion IRQ (disk I/O inside `sys_open`) hung forever because the satisfying IRQ could never fire.
8. **`proc_alloc()` marked a brand-new PCB `PROCESS_READY` before it was initialized** — harmless with no preemption, but the instant real preemption (#6/#7 above) and a second `process_create()` call could interleave, the scheduler could pick a half-built PCB mid-ELF-load and crash.
9. **PCB leak on every `process_create()` error path** — none of the four early-return failure paths reset `state` back to `PROCESS_UNUSED`, permanently leaking the slot on any failed creation.

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

**Current code reality check:** `struct process` today conflates the two — it holds exactly one `struct kcontext` (one execution context) and is what `schedule()` switches between. This is fine as the *first* increment (§13 Phase A keeps it this way deliberately) but §13 Phase D is where `struct thread` splits out, once anything needs more than one thread per process. Not doing this split prematurely is itself a design decision — see §17 Trade-offs.

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
              │  │ (Phase A:  │  │ (Phase B,   │  │ (Phase C, 🎯)   │ │
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

Every field above exists because the trampoline or the scheduler dereferences it at a specific moment — there is no incidental state:

- `pml4_phys` / `kstack_top` are read by `dispatch()` (§5) *before every single switch*, including the very first one, which is why `process_start_first` duplicates that logic rather than special-casing it away.
- `ctx` is opaque to everything except `kernel_ctx_switch`/`kernel_ctx_restore` — no other code should reach into it.
- `entry_point` / `user_rsp` are read exactly once, by `process_trampoline`, and never again — they describe how to *become* the process, not its ongoing state. (This is why they don't belong in `ctx`: `ctx.rip` is always `process_trampoline` for a process that hasn't run yet, and becomes the real resume address only after the first voluntary yield.)
- `wait_next`/`wait_queue` are read/written only by the wait-queue primitive (§7.3) and by `process_kill` (§9) when killing a blocked target — never touched directly by ad hoc code, per invariant 4 in §7.
- `fds` is embedded by value rather than a pointer to a separately-allocated table, because a process's fd table has the exact same lifetime as the PCB itself (created with the process, destroyed with it) — there is no case where it outlives or is shared independently of the PCB, so a separate allocation would just be an extra failure path with no benefit. `fs/fd.c`'s `fd_table()` helper returns `&current_process->fds[0]` when there is a current process, or a boot-time-only global array before any process exists (preserves pre-process selftest behavior).

### 6.2 Process Control Block — remaining additions (🎯, phased; see §13 for when each lands)

```c
struct process {
    /* ... fields above, unchanged ... */

    /* Phase C: priority scheduling */
    uint8_t  priority;             // 0 = highest; small fixed band count (§4.2)
    uint32_t ticks_since_scheduled; // aging counter, decays priority-starvation

    /* Phase D: parent/child tracking for a real wait() (see §7.4 — sys_wait today
       busy-polls process_find() + sys_yield rather than being woken by the child) */
    struct process  *parent;       // for orphan-collecting / wait() semantics
    struct process  *zombie_next;  // reaped-but-unclaimed children, singly-linked

    /* Phase E: thread split (only if/when multi-threading is needed) */
    // struct thread *threads;     // NOT added speculatively — see §17 Trade-offs
};
```

Each addition is annotated with the phase that needs it — nothing here is added "because we'll probably need it eventually." `docs/ARCHITECTURE.md` §1 explicitly warns against over-scoping (COW/`fork()` were deliberately pushed off the critical path); this table applies the same discipline field-by-field.

### 6.3 Run queue (Phase A today, generalizes through Phase D)

Phase A (✅ built, implicit): the run queue *is* `proc_table[MAX_PROCESSES]`, scanned linearly from `current_process`'s index forward. This is O(n) per `schedule()` call, which is irrelevant at `MAX_PROCESSES = 16` and will be revisited only if the table size grows by an order of magnitude (it won't, soon — see §13).

Phase C (🎯): a fixed array of intrusive ready-queue heads, one per priority band:

```c
#define SCHED_PRIORITY_BANDS 4     // e.g. REALTIME, INTERACTIVE, NORMAL, BACKGROUND

struct run_queue {
    struct process *head[SCHED_PRIORITY_BANDS];
    struct process *tail[SCHED_PRIORITY_BANDS];
};
```

Phase D (🎯, SMP): one `struct run_queue` **per CPU**, not one global one — see §8 for why a single global run queue is the wrong shape to reach for even before SMP lands, and why it's cheap to avoid now.

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
                          │ automatic (✅ Phase A/B, §7.4) — deferred
                          │ one schedule() call behind the exit;
                          │ sys_wait (✅) busy-polls + reaps; a real
                          │ parent-blocked wait() is still Phase D, 🎯
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
    struct process *head;   // intrusive via process::wait_next
};

void wait_queue_block(struct wait_queue *wq, struct process *p);  // READY/RUNNING -> BLOCKED
void wait_queue_wake_one(struct wait_queue *wq);                   // BLOCKED -> READY, one process
void wait_queue_wake_all(struct wait_queue *wq);                   // BLOCKED -> READY, all
```

Built exactly as specified — `process.c`'s `wait_queue_block`/`wait_queue_wake_one`/`wait_queue_wake_all`, plus a static `wait_queue_remove` helper used both by a normal wake and by `process_kill` (§9) unlinking a killed-while-blocked target. `process::wait_queue` (§6.1) records which queue a blocked PCB is currently on, so `process_kill` doesn't need the caller to already know.

This is deliberately *not* a condition variable in the pthreads sense (no associated mutex to atomically release-and-block) — because there is no SMP yet, "atomically" is free (interrupts-off is sufficient exclusion on one core). The API is intentionally written so that adding real mutual exclusion later (Phase D, SMP) means changing the *implementation*, not the call sites — see §8. **Not yet exercised by anything user-facing**: no syscall today actually blocks a caller on a `struct wait_queue` (`sys_wait` busy-polls instead, see §7.4) — the primitive is proven correct by `test sched roundrobin`'s blocking variant and is ready for the first real consumer (a blocking `sys_wait`, a pipe/port read, etc.), but nothing has needed it yet.

### 7.4 Termination and reclamation (✅ automatic reap built Phase A; ✅ pid-directed `process_reap`/`sys_wait` built Phase B; true parent-blocked `wait()` still Phase D 🎯 — closed Bug 3, §16)

Automatic reclamation (unchanged since Phase A):

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

Why the deferral needs exactly one pending slot, not zero and not more: reclaiming `prev` *inside the same `schedule()` call* that switches away from it would free memory the CPU is still executing on (the remainder of `schedule()`, including the `kernel_ctx_switch` call itself, runs on `prev`'s stack). Waiting until the *next* `schedule()` call guarantees a full switch has happened since — we're now provably on a different stack. One slot suffices because every exit calls `schedule()` itself, so the queue never grows past depth 1 in practice. **Now genuinely exercised** (previously only correct by construction, per §3): `test sched reap` spawns and exits well past `MAX_PROCESSES` in a loop and asserts it keeps succeeding, and real multi-process use via `sys_spawn`/`sys_wait` hits this path every time.

**Built in Phase B:** `process_reap(pid)` — looks up the PCB by pid and calls `process_reap_slot` on it if it's a zombie, used directly by `sys_wait`. `sys_wait(pid)` (`syscall.c`) is a real, working wait — but implemented as a busy-poll: it calls `process_find(pid)` + `sys_yield()` in a loop until the target reaches `PROCESS_ZOMBIE`, then reaps it and returns the exit code, returning `-EMBK_ECHILD` if the pid doesn't exist. This is correct (the zombie can't disappear from under it — nothing else reaps a pid the caller is actively waiting on) but not the eventual design: **still Phase D, 🎯** is a `sys_wait` that *blocks* the caller on a wait queue and is *woken* by the target's own exit, which needs `process::parent`/`zombie_next` (§6.2) so the exiting process knows who (if anyone) to wake, rather than every waiter spinning independently. Busy-polling was chosen as the correct, simple stand-in rather than building parent/child tracking before anything needed it — consistent with §17's general bias against speculative structure.

---

## 8. Synchronization

**Before Phase B:** none needed. Single core, and `schedule()` was only ever called from syscall context, never from an interrupt handler, so there was no reentrancy to guard against.

**Since Phase B (✅ preemption is real now):** two separate hazards, worth keeping distinct — (a) is now **partially, not fully** closed; (b) is still purely Phase D/future.

- **(a) Preemption reentrancy — partially closed, one gap still open.** When `schedule()` is entered *from the LAPIC timer ISR*, it's safe from being reentered by a second timer tick: the timer handler is an interrupt gate (`kcontext.asm`'s callers all are), so IF is 0 for its entire duration and no further timer IRQ can land until it returns — this is why Bug 6/7's fix (forcing IF=1 only in the *saved snapshot*, not in the live flags during the ISR) doesn't reopen this hole. **But** `schedule()` is also called from syscall context (`sys_exit`/`sys_yield` → `process_exit_self`/`sys_yield` → `schedule()`), and `syscall_dispatch()` unconditionally executes `sti` before dispatching (needed for Bug 7's fix, disk-I/O syscalls). That means a `schedule()` call triggered by `sys_exit`/`sys_yield` runs with IF=1 — a timer IRQ *can* land mid-dispatch inside that call and re-enter `schedule()` from the timer ISR while the outer, syscall-triggered `schedule()` call hasn't returned. This has not been observed to misbehave (no failing test currently exercises it), but it is a real, currently-open gap, not a Phase D hypothetical — the general fix is the standard one (bracket `schedule()`'s critical section with `cli`/`sti`, restoring the entry IF state on exit rather than unconditionally re-enabling), it just hasn't been built yet. Flagged here explicitly so it doesn't get mistaken for "solved because preemption works in the tests that exist."
- **(b) Cross-CPU run-queue access** — a genuinely different problem that interrupt-disabling does *not* solve, because it doesn't stop a different CPU from touching the same queue concurrently. This needs real locks (`cpu/spinlock.c` already exists in the tree, unused by this subsystem today). Still purely Phase D — no second CPU exists yet.

**Design decision for Phase D:** per-CPU run queues (§6.3), each with its own spinlock, rather than one global run queue behind one global lock. Reasoning:

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

---

## 10. Failure Modes

| Failure | Behavior before the fix | Now |
|---|---|---|
| Kernel stack overflow (Bug 4) | Silent heap corruption, manifests later as an unrelated-looking crash | ✅ Fixed. Unmapped guard page below every kernel stack → immediate `#PF` at the overflow site |
| Kernel-stack VA region invisible in a fresh process's own page tables (Bug 5) | N/A — introduced *by* the Bug 4 fix, in the same session; never shipped independently | ✅ Fixed. Region reuses `MMIO_BASE`'s already-populated PML4 slot instead of a fresh one — see §7.2/§16 |
| Zombie accumulation (Bug 3) | After exactly `MAX_PROCESSES` exits, `process_create` fails forever until reboot | ✅ Fixed (automatic reclamation, §7.4), now genuinely exercised by `test sched reap`. A true parent-blocked `wait()`/orphan-collecting PID 1 still Phase D |
| Runaway process (infinite loop, ignores its message port) | Previously *couldn't* be stopped — no preemption existed, so it never yielded the CPU at all | ✅ Fixed. Timer-driven preemption (§5, ✅) means it's evicted every quantum regardless of cooperation, and the uncatchable kill (§9, ✅) can force it to `ZOMBIE` outright — verified together via `test sched kill` (kills a process running an intentional infinite loop) |
| Double-free / use-after-reap | N/A, reaping didn't exist | Guarded now: `process_reap_slot` asserts `state == PROCESS_ZOMBIE` before freeing anything; reused PCB slots are always fully zeroed first |
| Scheduler picks a half-initialized `PROCESS_READY` PCB (Bug 8) | Possible the instant real preemption + a second `process_create()` call coexist — `proc_alloc()` marked slots `READY` immediately, before pml4/kstack/ctx were built | ✅ Fixed. `proc_alloc()` marks `PROCESS_BLOCKED` instead; only `process_create()`'s final step sets `READY`, after everything is actually built |
| Scheduler picks a `BLOCKED` process | Previously not possible (nothing set `BLOCKED`) | ✅ Structurally impossible now that §7.3 is built: the run-queue removal on block (§7.3's design rationale) means a blocked PCB simply isn't in the schedulable set, not "checked and skipped at schedule time" |
| RFLAGS.IF corrupted across a context switch (Bug 6) | N/A, no preemption existed to expose it | ✅ Fixed — `kernel_ctx_switch`/`kernel_ctx_save` force IF=1 in the saved snapshot (see Bug 6, §16) |
| Syscall hangs waiting on an IRQ that can't fire (Bug 7) | N/A, no syscall did blocking I/O before file-I/O syscalls landed | ✅ Fixed — `sti` at the top of `syscall_dispatch()` |
| PCB slot leaked on process_create() failure (Bug 9) | A failed creation (bad ELF, OOM) permanently removed a slot from the pool, with no PID ever having existed to explain why | ✅ Fixed — all four early-return error paths reset `state = PROCESS_UNUSED` |
| Timer IRQ re-enters `schedule()` while a syscall-triggered `schedule()` call is still in progress (§8(a)) | N/A, no preemption existed | **Still open.** Not yet observed to misbehave, but not actually prevented — `syscall_dispatch()`'s `sti` (needed for Bug 7) means `schedule()` calls from `sys_exit`/`sys_yield` run with IF=1. Needs `schedule()` to bracket its own critical section with `cli`/restore-IF-on-exit; not built yet |
| Two CPUs both believe they're running the same PCB (Phase D) | N/A, single core | Per-CPU run queues (§8) + an explicit `PROCESS_RUNNING` owner-CPU field, checked on dispatch |

---

## 11. Debugging Strategy

- The existing serial exception dump (used this session to diagnose Bug 1) prints vector, error code, `RIP`, `CS`, `RSP`, `SS`, `RBP` — sufficient to catch a bad `iretq` frame, as it did. **Extension for this subsystem:** on any exception where `CS` indicates ring 0 (a kernel-mode fault, like Bug 1 was — the fault happens *during* the privilege transition, still ring 0), additionally dump `current_process->pid` and `state`, so a future "which process's trampoline just crashed" question doesn't require a `gdb` session to answer from serial logs alone.
- `docs/GDB_CHEATSHEET.md` already documents the QEMU `-s -S` + `gdb` workflow (`make debug` target) — the natural tool for stepping through `kernel_ctx_switch`/`process_trampoline`, since bugs in this subsystem are exactly the kind (wrong register in wrong slot, wrong selector value) that a register dump at the fault site resolves in minutes, as it did for Bug 1 this session.
- **Proposed addition:** a `dump_process_table()` debug command (following the existing `selftests.c` `test <name>` pattern) that prints every PCB's `pid`/`state`/`priority` — the single most useful "is the scheduler stuck" diagnostic, and trivial to build once §7 lands.

---

## 12. Testing Strategy

Following the existing `selftests.c` convention (`test embkfs`, `test vfs`, `test fd`, `test ring3`, etc. — a shell command dispatches to a self-contained in-kernel check that prints pass/fail with a concrete assertion, not just "didn't crash"). **All four specified tests below are now built** (`process_test_roundrobin`/`process_test_kill`/`process_test_reap`/`process_test_stackguard` in `process.c`, wired into `selftests.c` as `test sched roundrobin`/`kill`/`reap`/`stackguard`), closing the gap this section originally flagged (everything through Bug 5 was manual QEMU + `gdb`/`objdump` only):

- **`test sched roundrobin`** ✅: uses `process_create_kthread` (ring-0 "threads" sharing the kernel's own PML4, built specifically so scheduler tests don't need a real ELF file) to spawn dummy processes that each increment a shared counter; asserts every process ran at least once within N scheduling rounds and none ran twice before all others ran once. Also exercises the wait-queue blocking path (§7.3).
- **`test sched kill`** ✅: spawns a kthread running an intentional infinite loop that never touches its message port; issues `process_kill`; asserts the PCB reaches `PROCESS_ZOMBIE` within one scheduling quantum. This is the test that actually proves §3.3's "day one" guarantee is real, not aspirational.
- **`test sched reap`** ✅: spawns and exits a process well past `MAX_PROCESSES` in a loop; asserts this keeps succeeding (proves Bug 3 stays fixed — a regression here is exactly "silent until it isn't," the failure mode this whole spec is trying to design against). This is also the first test to actually *exercise* the deferred-reap path (§7.4) — before it existed, nothing in the tree had ever created a second concurrent process to trigger it.
- **`test sched stackguard`** ✅: deliberately checks that the guard page below a kernel stack is genuinely unmapped (`vmm_get_phys` returns not-present) rather than actually recursing into it — there's no fault-recovery path yet, so the test proves the guard page *would* fault without needing to survive a real one.

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
2. ✅ Timer-driven preemption: `lapic_timer_handler` calls `schedule()` on every tick (100Hz). Safe from being reentered by another timer tick (interrupt gate keeps IF=0 for the ISR's duration) — but see §8(a) for a related reentrancy gap that's still open, involving `schedule()` calls made from syscall context instead.
3. ✅ Uncatchable kill (§9) — `process_kill(pid)`. This is the point at which `docs/ARCHITECTURE.md` §3.3's "ships with the scheduler" promise is actually redeemed, not just documented.
4. ✅ `test sched kill` (§12).
5. ✅ `test sched reap` (§12) — pulled forward from the original Phase D plan below once it became clear `test sched roundrobin`'s kthread infrastructure made it trivial to also spawn/exit past `MAX_PROCESSES` in a loop; no reason to wait for full lifecycle completion just to prove Bug 3 stays fixed.
6. ✅ `sys_wait`/`sys_spawn`/`sys_yield`/`sys_getpid` syscalls, per-process fd table (unblocks `spawn()`'s file-action model, §3.2) — also pulled forward from Phase D: once preemption and wait queues existed, a busy-polling `sys_wait` (§7.4) was simple enough to build immediately rather than wait for parent/child tracking. **Not yet done from the original Phase D scope:** a real blocking `sys_wait` (needs `process::parent`/`zombie_next`, §6.2) — still Phase D, tracked below.
7. ✅ Four new bugs found and fixed while building the above (Bugs 6–9, §16): RFLAGS.IF corruption in context switch, `int 0x80` leaving IF=0 for the whole syscall, the `proc_alloc()` READY-before-initialized race, and PCB leak on `process_create()` error paths.

**Phase C — Priority scheduling:**
1. Fixed priority bands + aging (§4.2, §6.3).
2. Revisit `dispatch()` (§5) to select across bands, not just round-robin the flat table.

**Phase D — Lifecycle completion + SMP shape (partially pulled into Phase B above; remaining scope):**
1. A real blocking `sys_wait` — replace the busy-poll (§7.4) with `process::parent`/`zombie_next` (§6.2) tracking so the exiting child wakes its waiting parent directly instead of the parent spinning on `sys_yield`.
2. Per-CPU run queues + spinlocks (§8) — built when SMP bring-up (AP init, per-CPU data) actually lands elsewhere in the kernel, not before; this phase is a placeholder marker, not a trigger.

**Phase E — `fork()` compat (only if a concrete program needs it):**
1. COW pages in the VMM (explicitly `docs/ARCHITECTURE.md` §3.2's territory, deferred until here).
2. `fork()` as a compat syscall built from `spawn()`'s address-space-construction path, not a parallel implementation.

**Explicitly not scheduled, revisit only on real need:** `struct thread` split (§4.1's deferred half), CFS/EEVDF-style fairness accounting (§4.2), PID reuse/namespace machinery (§7.1).

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
void process_start_first(void);                     // ✅ built — one-way, never returns
void process_exit_self(int code);                   // ✅ built — sys_exit's real body
                                                     //    (named _self, not process_exit:
                                                     //    it always acts on current_process,
                                                     //    there is no "exit some other pid")
void process_reap(uint32_t pid);                    // ✅ built — used directly by sys_wait
void process_find(uint32_t pid);                    // ✅ built — PCB lookup by pid, used by
                                                     //    sys_wait's busy-poll and process_kill

/* Scheduling */
void schedule(void);                                // ✅ built (Phase A/B: round-robin,
                                                     //    now timer-preemptible;
                                                     //    Phase C: priority-aware, same signature)
void sys_yield(void);                               // ✅ built, wired to syscall number 3

/* Blocking (Phase B) */
void wait_queue_block(struct wait_queue *wq, struct process *p);  // ✅ built
void wait_queue_wake_one(struct wait_queue *wq);                   // ✅ built
void wait_queue_wake_all(struct wait_queue *wq);                   // ✅ built

/* Uncatchable kill (Phase B) */
void process_kill(uint32_t pid);                    // ✅ built — forces ZOMBIE regardless of state

/* Scheduler selftests (Phase B), see §12 */
void process_test_roundrobin(void);                 // ✅ built
void process_test_kill(void);                        // ✅ built
void process_test_reap(void);                        // ✅ built
void process_test_stackguard(void);                   // ✅ built
```

### 15.2 Userspace-facing syscalls (✅ all built except `sys_kill`)

Per `docs/ARCHITECTURE.md` §3.4's split (fds for streams, typed handles for structural objects): process/thread handles are **not** file descriptors. §3.4/§3.5's eventual capability-handle model for `sys_spawn`/`sys_wait` is not yet built — **what actually shipped in Phase B uses a raw `pid_t`-shaped `uint32_t`, the plain/simple version**, not yet the opaque per-caller handle described below. This is flagged as a known divergence from the settled design, not an oversight: it was the pragmatic choice to get a *working* spawn/wait/kill surface before deciding the handle-scoping details, and should be revisited before untrusted code can call these syscalls (a raw pid lets any caller name any other process, which the handle model was specifically designed to prevent).

```
sys_exit(code) -> !                          // ✅ built (int 0x80, SYS_exit = 2)
sys_spawn(path) -> pid                        // ✅ built (SYS_spawn = 10) — builds on
                                               //    process_create; file_actions[] param
                                               //    and the handle-vs-pid model both
                                               //    still 🎯, unscheduled (see above)
sys_wait(pid) -> exit_code                    // ✅ built (SYS_wait = 11) — busy-polls
                                               //    rather than blocking via wait_queue;
                                               //    see §7.4 for why and what's still 🎯
sys_yield() -> void                           // ✅ built (SYS_yield = 3)
sys_getpid() -> pid                           // ✅ built (SYS_getpid = 12) — not in the
                                               //    original spec, added because a spawn()
                                               //    caller needs to tell itself apart from
                                               //    a child running the same binary
sys_kill(handle) -> void                      // 🎯 still not built — the *userspace-reachable*
                                               //    edge of the uncatchable kill; note the
                                               //    kernel-internal path (process_kill) is
                                               //    already fully built and used by the
                                               //    selftests, just not exposed to ring 3 yet
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

---

## 17. Trade-offs (explicit, not buried in the prose above)

| Decision | What we gave up | Why it's still right, for now |
|---|---|---|
| Round-robin, not priority/fair-share (§4.2 Phase A/B) | Interactive responsiveness under contention | Real preemption now exists (Phase B), so contention is no longer hypothetical, but `MAX_PROCESSES=16` on a single-user workstation still doesn't produce enough runnable processes at once for fairness to matter in practice; building fairness accounting now is still solving next year's problem with this year's guesses — revisit if it's ever actually observed to matter |
| `sys_wait` busy-polls instead of blocking on a wait queue (§7.4) | CPU cycles spent spinning/yielding while a parent waits, instead of the caller being genuinely off the run queue | The wait-queue primitive (§7.3) exists and this could use it, but doing so needs `process::parent`/`zombie_next` (§6.2) so the exiting child knows whom to wake — busy-polling is a correct, much simpler stand-in that didn't require building parent/child tracking just to ship a working `wait()`; revisit when that tracking is needed for something else anyway (Phase D) |
| `sys_spawn`/`sys_wait` take a raw `uint32_t` pid, not an opaque capability handle (§15.2) | The confused-deputy protection `docs/ARCHITECTURE.md` §3.4/§3.5's handle model was designed to provide (a raw pid lets any caller name any other process) | Shipping the plain pid-based version first let spawn/wait/kill land as a coherent, testable unit without also having to design the handle-scoping rules in the same pass; explicitly flagged (§15.2) as needing revisiting before untrusted code can call these syscalls, not a forgotten detail |
| Process/thread kept unified for now (§4.1, deferred split) | The "correct" separation from day one | A process with one thread is the only case that exists; splitting early means carrying an unused abstraction through every change until something actually needs 2 threads |
| Static `MAX_PROCESSES` array, not a dynamic allocator (§6.3) | Scaling past 16 concurrent processes | A dynamic PCB allocator is real work (allocation failure paths, iteration without a fixed bound) that a single-user dev workstation doesn't need yet; revisit if 16 is ever actually hit in practice, not before |
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
