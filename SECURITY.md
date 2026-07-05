# Security

EmbLinkOs is an **operating system under active development**, built
from scratch to understand every layer. It is **not production software** and is
not intended to run untrusted workloads on real hardware. It runs in QEMU during
development. There are no releases, no supported versions, and no users to
protect — so the usual "supported versions / report a vulnerability" process
does not apply.

What this document *does* cover: the security **properties the kernel enforces**,
and the **known gaps** that are consciously deferred. In an OS, "security" is a
concrete technical boundary (privilege levels, address-space isolation), so this
is a map of what isolation actually holds today.

## Threat model (current)

The relevant boundary today is **kernel (ring 0) vs. user (ring 3)**: a user
program must not be able to read/write kernel memory, corrupt kernel page
tables, or escalate privilege. Multi-user / multi-tenant isolation, network
attack surface, and hardware side-channels are **out of scope** at this stage.

## Enforced properties

- **Ring 0 / ring 3 separation.** User code runs at CPL 3; privileged
  instructions fault (#GP). Verified: `cli` from ring 3 traps.
- **Per-process address-space isolation.** Each process has its own PML4; the
  user half (PML4 slots 0–255) is private, the kernel half (256/511) is shared
  read-through. A process cannot name another process's user memory — the PML4
  index is a pure function of the address, and user/kernel occupy disjoint slot
  ranges by construction.
- **User load address is bounded.** The ELF loader rejects any `PT_LOAD` with a
  higher-half `p_vaddr`, so a crafted binary cannot map over kernel page-table
  slots.
- **W^X.** Code pages map R-X (no write), data/stack pages map R-W + NX (no
  execute), enforced per ELF segment via `p_flags`. Verified: a write to a code
  page from ring 3 faults #PF (W=1).

## Known gaps (deliberately deferred — tracked in `docs/TODO.md`)

These are *known* and *tracked*, not hidden. Naming a limitation is part of the
project's discipline.

- **Syscall user-pointer validation.** `sys_write` (and future syscalls)
  dereference user-supplied pointers without an `access_ok`/`copy_from_user`
  bounds check. A user pointer into kernel space would be trusted. Fix depends
  on the per-process ranges now in place; mandatory before running untrusted
  programs.
- **Address-space teardown.** No `vmm_destroy_address_space` yet — a failed load
  or a process exit leaks its page-table pages and frames. Not an isolation
  hole, but a resource-exhaustion one once many processes come and go.
- **EMBKFS crash-orphan.** An unlinked-but-open file crashed mid-life leaks its
  inode (no on-disk orphan list; mount-time sweep planned). Space leak, not
  corruption.

## Reporting

This is a solo for now but I will like to make it collaborative with any one who wants to contribute (github.com/teo1747/EmbLinkOs). If you're reading
the code and spot a security-relevant bug — an isolation break, a way for ring 3
to reach kernel memory, a fault the kernel mishandles — open a GitHub issue using
the `[bug]` template (include the fault dump). There's no formal disclosure
process; it's just useful feedback on an OS being built to learn.
