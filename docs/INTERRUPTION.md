# Interruption on EmbLink — design

*Status: **ALL THREE PHASES IMPLEMENTED AND VERIFIED LIVE.**
`cancel_test: PASS`; `test ctrlc` (real Ctrl+C -> routed child cancelled, exit
42); `test ctrlc2` (a DECLINING child -- an embk_sleep_ms loop that never
observes the flag -- is killed after the shell's 1500ms grace; before the pump
was made pollable this test hung forever, so completion is the assertion).*

How does a user stop a running command, and how does a program get the chance to
clean up first? Unix answers "signals". This document argues EmbLink should not,
and proposes what it should do instead.

---

## 1. The need (and only the need)

Three concrete things, no more:

1. **A user presses Ctrl-C and the running command stops.** Today nothing can
   interrupt a running process at all.
2. **A program gets the chance to clean up.** git removes `.lock` files; a
   half-written index is worse than no index.
3. **A wedged program can still be stopped**, whatever it thinks about it.

Note what is *not* on the list: arbitrary process A interrupting arbitrary
process B; 32 numbered conditions; timers, child-death notification, or job
control. Those are Unix's answers to other questions and should be argued for
separately, on their own merits, if ever.

---

## 2. Why not POSIX signals

Signals are three separable things fused together. Each part conflicts with a
position EmbLink has already taken and paid for:

| POSIX signals | EmbLink |
|---|---|
| **Ambient authority** — any process may signal any pid it can name | Authority is a **capability you were handed**: `embk_kill(handle)`, obj-handles, file-actions naming every fd, the environment passed explicitly ([[environment-explicit-at-spawn]]) |
| **Asynchronous control-flow injection** — a handler runs on the interrupted stack, between any two instructions | Every other EmbLink event surfaces at a point the program already checks: a syscall return, a read, a poll |
| **A fixed global namespace** — 32 numbers with baked-in meanings | Objects are **typed and named**: pipes, channels, endpoints, surfaces |

The async-injection half is the expensive one. It is why `EINTR` exists, why
"async-signal-safe" is a category of function, why `malloc` is not in it, and why
signal handlers are a perennial source of bugs. It buys exactly one thing: waking
a thread blocked in a syscall. **EmbLink can buy that directly, without the
injection**, by having the syscall return.

There is also a live precedent for getting this exactly wrong in the other
direction. This OS already refuses `fcntl(F_GETFD)` — and that refusal was a
*bug*, because `FD_CLOEXEC` was **vacuously true** here (no exec ⇒ no fd ever
survives into an exec'd image). The lesson recorded in [[cpython-port]]:

> A refusal is only honest if the capability is genuinely ABSENT. Ask: is this
> MISSING, or TRIVIALLY TRUE here?

Applied to signals, the answer splits, and the split is the whole design:

- **SIGPIPE is vacuously satisfied.** Our pipe write returns `EPIPE` and raises
  nothing — which is precisely what `signal(SIGPIPE, SIG_IGN)` buys on Unix.
  Nothing to build.
- **`raise`/`abort` already work.** newlib dispatches a self-signal
  synchronously; `SIG_DFL` routes through our `kill(self)` → `_exit(128+signo)`.
  A self-signal is a *call*, not an interruption. Nothing to build.
  ([[signals-self-delivery-only]])
- **SIGINT is NOT vacuously satisfied.** The handler never runs only because we
  cannot deliver. That is the real gap, and it is the only one.

So the job is not "implement signals". It is **"deliver an interruption"** — once,
for one condition, and we get to choose the shape.

---

## 3. What already exists (build on it, don't duplicate it)

- **`embk_kill(handle)`** — uncatchable termination of a child you hold a HANDLE
  for. Capability-scoped. **The shell already holds its child's handle.**
- **`embk_proc_kill(pid)`** — ambient, deliberately: the shell's `kill` builtin,
  documented as the single-user concession.
- **`FD_BACKING_CONSOLE`** — the console is already a *typed* fd backing
  (kernel/fs/fd.h), not an untyped device number.
- **`embk_fd_avail(fd)`** — a readiness probe (SYS 52).
- **Pipes, channels, endpoints** — a real IPC story already exists.

### The trap in the obvious designs

Two "obvious" answers both fail, and it is worth writing down why:

- **"^C goes to whoever holds the console."** Doesn't discriminate: stdio 0/1/2
  is *inherited by default* (kernel/fs/fd.c ~:225) — the shell **and** its child
  both hold the console.
- **"^C goes to whoever is blocked reading the console."** Precise, and useless
  for the case that matters: a compute-bound child (`git clone` grinding) is not
  reading stdin, so there is no target at the exact moment you want one.

Both fail for the same underlying reason: they try to *infer* who the foreground
process is. **EmbLink does not infer authority. It is handed over.**

---

## 4. The proposal

### 4.1 Interrupt routing is a delegation, not an inference

There is no "foreground process", no session, no process group. Instead:

> **A process may name ONE target to receive console interrupts on its behalf.**

```c
int embk_console_interrupt_route(int handle);  /* handle from spawn, or -1 to reclaim */
```

The shell holds the console interrupt right (it is the console's reader). When it
spawns a foreground command it **delegates**: "^C goes to that child". When the
child exits, the shell reclaims it (`-1`).

This is the same move the OS already makes everywhere: a child gets an
environment because the parent named one; a child gets fd 3 because a file-action
named it; a child receives interrupts because the shell routed them. **Nothing is
inferred and nothing is ambient.** A process that never routes anything is simply
never interrupted, which is a true and predictable statement about it.

Delegation is one level deep on purpose. A shell running a pipeline routes to the
stage it chooses; nested shells re-route for their own children. No tree walk, no
"process group" concept, and no way to interrupt something you were not handed.

### 4.2 The effect: cancellation, observed — never injected

^C does **not** run a handler. It sets a **cancel flag** on the target process,
and:

- **Blocking syscalls return `-ECANCELED`.** Blocked reads/writes/waits wake and
  fail. This is the async-wake that signals were carrying, without the injection:
  the program learns at a point it *already* checks — a syscall return.
- **`embk_cancelled()`** — a compute loop with no syscalls can poll it.
- The flag is **sticky**: once cancelled, subsequent blocking calls keep failing.
  A program cannot accidentally "miss" it by being between calls, and cleanup
  code that itself blocks would deadlock if the flag auto-cleared.

Why `ECANCELED` and **not** `EINTR`: `EINTR` means *"restart me"*, and correct
POSIX programs loop on it — which would defeat the cancellation entirely.
`ECANCELED` means *stop*. Programs treat an unexpected I/O error as fatal, unwind,
and exit — which is exactly the cleanup we want, through code paths they already
have and already test.

**Cleanup is best-effort, and honestly so.** A cancelled program may ignore the
flag forever. That is why:

### 4.3 Escalation is the parent's, and it is uncatchable

The shell's policy, in userspace, with tools it already has:

```
^C  ->  kernel sets the child's cancel flag  (polite; the child may clean up)
        shell waits a grace period
        child still alive?  ->  embk_kill(handle)   (uncatchable, already exists)
```

This is SIGTERM-then-SIGKILL — but with **no ambient authority anywhere**: the
shell can only escalate against a child whose handle it holds, and the grace
period is shell policy, visible and tunable, not kernel law.

### 4.4 What the kernel owes

Small, and mostly plumbing that exists:

- `struct process`: a `cancelled` flag.
- The console driver: on `0x03`, set the flag on the routed target (if any) and
  wake its wait-queues.
- Blocking syscalls: check the flag on entry and after every wake; return
  `-ECANCELED`.
- Two syscalls: `console_interrupt_route(handle)`, `cancelled()`.

Wake-up is the only genuinely fiddly part: every wait-queue sleeper must
re-check. That is a known, boring, *synchronous* correctness problem — as opposed
to building signal frames on a user stack, which is neither.

---

## 5. What this deliberately does NOT do

Stated so nobody reads the gaps as oversights:

- **No handlers.** No `SIGINT` handler will ever run. `signal(SIGINT, h)`
  continues to be accepted-but-never-delivered, which stays honest **only because
  nothing can deliver it** — and this design keeps it that way by never
  delivering a *signal*. If that becomes untenable, the fix is to make
  `signal(SIGINT, ...)` **refuse**, not to fabricate delivery.
- **No process-to-process interruption.** `kill(other, sig)` stays ENOSYS. Only a
  parent may cancel a child it holds a handle for, and only the console's routed
  target receives ^C.
- **No job control, no SIGTSTP/SIGCONT, no process groups.** Not needed for the
  three requirements in §1.
- **No EINTR/restart semantics.** Deliberate; see §4.2.

---

## 6. Consequences for the ports

- **git**: a cancelled `read`/`write` returns `ECANCELED`; git's existing error
  paths unwind and remove `.lock` files. It gets its cleanup without EmbLink
  growing signal handlers.
- **CPython**: `KeyboardInterrupt` is raised from a signal handler on Unix, so it
  will **not** appear. A cancelled Python instead sees an `OSError` from its
  current syscall and unwinds. Honest, but worth knowing before someone files it
  as a bug.

---

## 7. Phasing

1. ✅ **DONE.** `struct process.cancelled` (sticky; reset in process_alloc --
   slots are REUSED, so a stale `true` would make a process born cancelled),
   `process_cancel(pid)` (wakes each blocked thread via its OWN wait_queue --
   siblings may be blocked on different objects), `sys_cancel` 59 (handle-scoped,
   like sys_kill) + `sys_cancelled` 60, `embk_cancel()`/`embk_cancelled()`, and
   `-EMBK_ECANCELED` from `process_wait` and pipe read/write.
   **Ordering rule, in every check:** a completed operation WINS -- EOF and EPIPE
   are real, permanent answers and are tested BEFORE the cancel check. Cancelling
   must never turn a finished operation into a failure. The check sits just
   before the block, so a process cancelled while already asleep re-checks on the
   wake `process_cancel()` gave it.
   Verified live (`test ring3 threads` -> `cancel_test: PASS`): a child blocked
   reading a pipe nobody writes wakes with -ECANCELED, tells it apart from EOF(0),
   and finds the flag still set afterwards.
   ⚠️ Prerequisite found the hard way: `ECANCELED` was arriving as
   `EADDRNOTAVAIL` -- see [[errno-kernel-newlib-mismatch]].
2. ✅ **DONE + VERIFIED LIVE** (`test ctrlc`, Ctrl+C injected via QMP sendkey —
   the one half software cannot fake). The route slot lives in keyboard.c
   (`g_console_int_target`; ONE slot, one console, last-writer-wins — a stated
   single-user concession). `sys_console_interrupt_route` 61 accepts only a
   HANDLE the caller holds; -1 reclaims. The ^C check runs BEFORE sched_lock()
   in keyboard_deliver — process_cancel takes g_sched_lock itself, and this is
   IRQ context: taking it twice would self-deadlock. Unrouted ^C falls through
   as an ordinary byte rather than being swallowed.
   **Found on the way: the PS/2 driver never tracked Ctrl at all** — Ctrl+C
   produced a plain 'c', so ^C was not merely unrouted but UNTYPEABLE. Both
   Ctrl keys (0x1D/0x9D, and 0xE0-prefixed right Ctrl) now tracked; Ctrl+letter
   maps to the C0 code (only for letters, so Ctrl+digit stays ordinary).
   `console_fd_read` now blocks via `keyboard_getchar_blocking_cancelable()`
   (a buffered key still wins over the flag — the completed-operation rule);
   the plain blocking getchar stays for the kernel shell, which has no process
   context to cancel.
3. ✅ **DONE + VERIFIED LIVE** (`test ctrlc2`). The shell delegates on spawn,
   reclaims unconditionally after the wait, and now runs the grace-then-kill
   policy (`cancel_watch`, SHELL_CANCEL_GRACE_MS = 1500ms, eval_extern.c).
   What closed the gap: the pump's final drain was a plain BLOCKING pipe read
   -- for a command with no piped input the shell spent the child's entire
   runtime there, so no shell code could ever escalate. The drain is now
   fd_avail-polled (10ms sleep when quiet, matching the interleaved half that
   was already polled), ticking the policy while idle.
   The clock starts from OBSERVING the child (`embk_child_cancelled(handle)`,
   syscall 60 extended to handle-or-minus-one) -- the shell never sees the ^C
   itself, and without the observation "cancelled but declining" would be
   indistinguishable from "healthy but slow", forcing a choice between never
   escalating and killing innocent children.
   Safe-drain invariant this leans on: fds are released AT EXIT, not at reap
   ("a zombie holds no fds", process.c), and embk_kill drives that same
   last-thread transition -- so once proc_alive reports dead, a blocking drain
   returns trailing bytes then EOF and cannot hang. Frames a child completed
   before dying are still delivered: killing it must not un-happen its output.
   ⚠️ ABI note: sys_cancelled now READS rdi; embk_cancelled() passes -1
   explicitly (it was syscall0 -- garbage rdi would occasionally have been a
   valid handle).

Each phase is independently testable, and phase 1 is useful on its own (a
supervisor cancelling a child needs no console at all).
