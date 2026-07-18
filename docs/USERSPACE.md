# EmbLinkOS Userspace Design — the namespace, the layout, and why

**Status:** COMPLETE — all four decisions ratified 2026-07-18; tree derived in §6
(supersedes the same-day "split" ratification — see §2.6 for why).
Decisions are made from invariants up; the directory tree is the LAST
section of this document, derived from the decisions — never the
starting point. Method mirrors the kernel: no structure exists here
because Unix has it; every divergence and every conformance carries its
reason inline.

---

## 1. Ground truth — audit of 2026-07-18 (master @ 28dd89b + tty)

What is already in the ground. These are the constraints any layout must
either honor or explicitly migrate.

### 1.1 Mounts (kernel/main.c boot chain, kernel/fs/vfs.c)

| mount point | fs     | nature                                          |
|-------------|--------|-------------------------------------------------|
| `/`         | EMBKFS | the boot image; CoW, snapshots, Merkle-verified |
| `/fat32`    | FAT32  | secondary volume (compat/testing)               |
| `/run`      | epfs   | ephemeral, RAM-backed; exists SOLELY for rendezvous names |

The VFS mount table is real (8 slots, `vfs_mount()` composes live
volumes at path prefixes). The layout may therefore *mount* regions,
not only mkdir them.

### 1.2 Hardcoded paths (grep of kernel/, shell/, user/, boot/)

- **Rendezvous:** `/run/compositor`, `/run/compositor-shell`
  (endpoint.h, init.c, embk.h). Names you CONNECT to — not readable
  files. **This is the seed of the whole namespace model — see §2.2.**
- **Loader:** `/libembk.so` (elf.c:307 — the dynamic linker's one
  hardwired library path).
- **Boot spawn:** kernel launches `/home.elf` directly (main.c:551);
  `/init.elf` is a TEST binary (usermode.c), not a production init(1).
  There is currently NO init process in the boot path. (§2.4 makes one
  architecturally necessary.)
- **Flat root executables:** `/shell.elf`, `/tcc.elf`, `/git.elf`,
  python, tools — all at `/`. Shell resolution rule (eval_extern.c:92):
  bare `foo` → `/foo.elf`; absolute path as-is; **no PATH concept**.
- **Toolchain scratch at root:** `/crt0.o`, `/syscalls.o`, `/t.o`,
  `/l.elf` (selftests).
- **`/dev/null`:** the ONE special name, matched in `open()` BEFORE VFS
  resolution (fd.c:327). Its comment is load-bearing: *"there is no /dev
  directory to resolve through, and creating one on disk to host a fake
  file would be the dishonest version of this."*
- **Absent by inspection:** no `/bin/sh`, no default `PATH`, no `_PATH_*`
  anywhere in kernel, shell, or the newlib port glue. The POSIX compat
  surface is UNCLAIMED — a free choice, not a debt already incurred.

### 1.3 Filesystem capabilities (what the layout gets to use)

- **Symlinks + hard links: YES.** `embkfs_symlink_path`/`readlink`,
  `S_IFLNK`, `DT_LNK`, per-object `links` count. A compat view can be
  real links, not a search-path fiction. (To verify when first used:
  whether the VFS auto-follows links on `open()`.)
- **Creation: YES.** `embkfs_create`/`mkdir` with B-tree split/merge;
  `O_CREAT` at the fd layer. The tree can be grown at runtime, not only
  baked into the image.
- **Snapshots + Merkle verification: YES.** CoW snapshots
  (`embkfs_snapshot_create`), CRC32C Merkle chain, verified boot path
  (`test embkfs verifyboot`). Asymmetric signing of the verified root is
  designed, deferred.
- **Device nodes: NO — deliberately.** No mknod, no devfs. Under §2 this
  stops being a gap: a device is reached by RESOLUTION to a typed
  handle, never by a fake inode.
- **Path limits:** kernel `SYSCALL_PATH_MAX = 256`; shell external-spawn
  buffer 128. Layout depth must stay comfortably inside these.

---

## 2. Decision 1 (revised) — a UNIFIED TYPED NAMESPACE, held per process

**Paths are for discovery. Typed handles are for interaction. The
namespace a process sees is itself a grant.**

### 2.1 The model

Internally the kernel is split subsystems (fs, process, memory, device,
IPC, compositor, …), each owning its object types. Externally there is
ONE namespace. The VFS is the *naming layer*, not the owner of every
object: it resolves a path far enough to identify the owning subsystem,
dispatches, and the subsystem returns a TYPED HANDLE. From that moment
all interaction is through the handle's own API — path operations are
over.

```
application → path lookup → VFS → owning subsystem → typed handle
```

- `/data/teo/readme.txt` → filesystem → file handle (byte stream: an fd)
- `/services/compositor` → compositor → channel handle
- `/proc/42`             → process mgr → process handle   (future)
- `/dev/display0`        → device mgr  → display handle   (future)

Nothing is coerced to bytes. A path that names a non-byte object does
not pretend to be a file; `open()` on it is a TYPE ERROR (§2.4).

### 2.2 Why — and why it supersedes the morning's "split" decision

1. **The seed is already live.** `/run/compositor` + `SYS_chan_connect`
   IS path → VFS → subsystem → typed handle. The revision does not
   contradict the ground; it promotes the working corner case to the
   general rule. The earlier "split" decision had scoped the namespace
   to byte-backed things and treated this as an exception; the better
   reading is that it was the prototype.
2. **The Linux unified byte-fs (`/proc` as text) stays rejected:** it
   reintroduces parse-structure-out-of-bytes — the disease the
   structured shell exists to cure — inside the kernel's own namespace.
3. **This is not Plan 9 either:** Plan 9 buys uniformity by forcing
   every resource to answer read/write/readdir. EmbLinkOS keeps objects
   frankly typed; uniformity lives in NAMING, not in a lowest-common-
   denominator byte interface. Closest living relatives: the NT Object
   Manager namespace (one tree of typed kernel objects; the filesystem
   is just one subsystem mounted into it) and Fuchsia namespaces (paths
   resolve to capability connections).
4. **The fd.c honesty principle survives intact.** What it condemns is
   faking BYTE FILES for non-byte objects. Resolution that returns a
   frankly-typed handle fakes nothing.

### 2.3 Authority: PER-PROCESS namespaces (naming is owning)

If any process could resolve `/proc/42` into a process handle, every
process would hold authority over every process — the confused-deputy
protection of the handle ABI, collapsed through the VFS back door. Two
coherent answers exist: checked resolution (global tree + access checks
— the NT road) and per-process namespaces (the visible tree is itself a
capability set — the Fuchsia road). EmbLinkOS takes **per-process**,
because three settled decisions already are that model:

- **spawn inherits nothing.** cwd, env, stdio, objects — all passed
  explicitly (file actions, ACTION_INSTALL_OBJ). A per-process
  namespace is the same principle applied to names: the tree you see is
  one more thing your parent handed you.
- **kill takes a handle, not a pid.** Authority flows from having been
  GIVEN it; the one pid-based exception (SYS_proc_kill) is marked
  "DELIBERATE AMBIENT AUTHORITY" in the source. The namespace obeys the
  same law rather than becoming the ambient exception that swallows the
  rule.
- **Confinement by construction composes.** A parent hands a child a
  NARROWED tree: a sandboxed build tool sees `/data/project` + a tmp
  dir and nothing else. No display service in your namespace = no
  display — absence, not "denied". Checked resolution is ambient
  authority plus guards: the identity-check model the capability half
  of the system exists to escape.

Resolving `/dev/display0` grants the display PRECISELY BECAUSE a parent
placed it in this process's tree: the grant was deliberate.

### 2.4 Staging — decided now, built later (the spawn/fork move again)

**Today's single global mount table is the degenerate case: every
process holds the full boot namespace.** Nothing is rebuilt now. What
the decision binds immediately is API SHAPE:

- `open()` remains byte-streams-only, forever. Resolution of non-byte
  objects goes through a future `resolve()`/`connect()` syscall
  (generalizing SYS_chan_connect) returning `(type, handle)`.
- `spawn` will grow NAMESPACE ACTIONS alongside file actions (bind a
  subtree, bind a service name, omit everything else). Not built until
  a confinement user exists.
- Subsystems register typed regions with the VFS when they have one to
  offer (`/proc`, `/dev`, `/services`). None exists yet; none is faked.
- **Bootstrapping consequence: a real init(1) becomes architecturally
  necessary** — the first holder of the full namespace, which delegates
  narrowed trees downward. The kernel currently spawning /home.elf
  directly is the placeholder. Administration is a privileged FULL VIEW
  (the supervisor's namespace), not a bypass.
- Enumeration of typed regions as structured tables (`ls /proc` → a
  table the pipeline can query) is the synthesis layer: explicitly
  deferred until "query the system with the pipeline algebra" is an
  active goal.

### 2.5 Consequences for names in the ground

- `/run` stays: ephemeral rendezvous names on an ephemeral fs — a
  rendezvous that survived reboot would lie about a dead connection.
  Long-term, service names belong under a typed `/services` region;
  `/run/compositor` migrates when `resolve()` exists.
- `/dev/null` stays a special name for now (compat spelling, honest
  implementation). Under the full model it becomes a legitimately
  RESOLVED null device — the special case dissolves instead of
  spreading. (Noted in fd.c: fd_open_into still lacks the special
  case; add when a spawn wants to silence a child's stdio.)
- No fake inodes, ever: a typed region exists only when its subsystem
  actually registers resolution for it.

### 2.6 What would reopen this decision

Evidence that per-process namespace state is unaffordably heavy at
spawn time, or that the two-syscall surface (open vs resolve) forces
pervasive duplication. Neither is expected; both are falsifiable.

---

## 3. Decision 2 — the sealed/mutable boundary: THE OS OWNS THE ABI

**Design invariant: the OS owns the ABI; compilers target the ABI.**
The sealed system is the operating system's trusted identity — the
definition of what it means to target EmbLinkOS. Compilers are
applications.

### 3.1 Sealed (updated only as a system update; future signed root)

- Kernel image + boot chain.
- The native ABI surface: `libembk.so` (the loader's one hardwired
  library), startup objects (`crt0.o`, `syscalls.o`), system headers.
  These ARE the definition of "targeting EmbLinkOS"; they evolve with
  the kernel, together, as one identity.
- The system's own programs — init (§2.4 makes it necessary), the
  compositor, the native shell. A system whose compositor is mutable
  state would attest to only half of itself.

### 3.2 Mutable state (everything touched in normal operation)

- User files and app data.
- **All installed applications — including every compiler.** TCC,
  Clang, a future GCC: apps that target the sealed ABI, installed /
  upgraded / replaced by file operations, never by re-blessing the
  system. Likewise python, git, and all ports.
- Toolchain scratch (today's `/t.o`, `/crt0.o` copies at root): TCC
  READS sealed artifacts (read-only reach into the system region) and
  WRITES products to state. The flat-root scratch was this boundary,
  unnamed and violated.

### 3.3 Why

1. **"Compatible compiler" becomes well-defined.** The sealed image
   contains the contract (headers, crt, libembk); any compiler
   consuming it and emitting conforming binaries is a valid citizen.
   Swapping TCC for Clang re-defines nothing.
2. **Stronger self-hosting, not weaker.** The machine's defining
   capability is not "contains tcc" but "publishes its own ABI and can
   host any conforming compiler."
3. **Attestation attaches to artifacts, not builders.** A self-rebuild
   PRODUCES a new sealed image; ADOPTING it is an update event
   (snapshot swap, future root re-signing) — regardless of which
   unsealed compiler built it. What is verified is what runs.
   (Trusting-trust — whether you trust the builder — is a distinct
   problem; no placement of the compiler on either side of this
   boundary solves it, and the seal does not claim to.)
4. **Update semantics fall out correctly.** TCC upgrade = file ops in
   state. `libembk.so` upgrade = system update, because it is the ABI —
   the kernel32/system-SDK shape, derived rather than copied.

### 3.4 Staging (the spawn/fork move, again)

The boundary is drawn NOW as the top-level split of one volume:
a system region and a state region, `/run` ephemeral beside them.
Enforcement arrives when verified-root signing lands: the system region
migrates to its own sealed EMBKFS volume (multivol is tested) and the
mount lands exactly where the directory was — ZERO path changes. Until
then the boundary is convention, and whole-volume verification churn on
state writes is accepted as a known, temporary cost.

### 3.5 What would reopen this decision

Evidence that the ABI surface cannot be crisply enumerated (header
sprawl bleeding into app-land), or a system program that genuinely
requires mid-life mutation outside the update path.

## 4. Decision 3 — native regions + the compat seam

**Names: `/system` (sealed), `/data` (mutable state), `/run`
(ephemeral, unchanged).** Self-describing, and carrying none of the
Unix false-friend baggage (`/usr` meaning not-user, `/home` beside a
`/root` that isn't root's). Everything below derives from §§2–3.

### 4.1 Derived structure

- `/system/bin` — the system's own programs (§3.1): init, the native
  shell, compositor, home.
- `/system/lib` — `libembk.so`. The loader's one hardwired path
  (elf.c:307) moves here: a one-line change.
- `/system/abi` — headers + `crt0.o` + `syscalls.o`: the targeting
  contract of §3.1, named as what it IS rather than scattered
  Unix-style across include/lib.
- `/data/apps/<name>/` — one directory per installed application (tcc,
  python, git, …): the app owns its binary and its files. Kills the
  flat-root sprawl; makes §3.3's "TCC upgrade = file ops" literally
  true — replace one directory.
- `/data/tmp` — scratch. Where `/t.o` goes to die.

### 4.2 Shell resolution — NO PATH in the native world

Bare `foo` resolves against an ordered, shell-owned list: `/system/bin`,
then `/data/apps/<name>/` entry points. There is no `PATH` environment
variable natively: PATH is ambient, mutable-by-anyone resolution
authority — the exact shape §2.3 rejects — and under per-process
namespaces the namespace ITSELF is already the authority over what is
reachable; a second, env-string-shaped authority would be redundant and
weaker. The search list is shell policy, visible in one place
(eval_extern.c). `PATH` the env var returns ONLY inside the POSIX
compat layer, for foreign software that reads it.

### 4.3 The compat seam — a reserved, currently-empty commitment

`/bin`, `/usr`, `/tmp` do not exist until foreign software (dash, make)
demands them — and then as a VIEW: symlinks into `/system/bin` and
`/data/apps`, `/tmp` → `/data/tmp`, populated only with the names
foreign software actually hardcodes, never mirroring the native layout.
`/dev/null` stays a special name until it resolves honestly (§2.5).

### 4.4 Migration

One dedicated, mechanical commit after this document is ratified —
image build script, elf.c loader path, eval_extern.c resolution,
kernel boot-spawn paths, selftest paths — not incremental drift that
leaves the tree half-old for weeks.

### 4.5 What would reopen this decision

Per-app directories becoming painful when two apps genuinely share a
runtime dependency (no shared-lib story between apps exists yet);
revisit when a second consumer of a shared library is real.

## 5. Decision 4 — user space proper

**A person's world is `/data/users/<name>/` — today, `/data/users/teo/`.**

- `users/` plural despite a single-user system: the multi-user shape
  costs nothing now, and a flat `/data/teo` would be a migration later.
  Regions now, machinery later — the same move as §2.4 and §3.4. What
  is deliberately NOT built: login, per-user identity, any of it. One
  user, root-equivalent, structure ready for the day that changes.
- **App data stays with the app** (`/data/apps/<name>/`, §4.1).
  PER-USER app state (the `~/.config` problem) is deferred until
  multi-user is real; when it comes, it lives under the user's world,
  not as hidden-file sprawl. A sentence now, a structure then.
- **Ownership is the three-layer model already built, stated once:**
  uid/gid/mode on disk (persistence + Unix compat), typed handles
  (authority over objects), per-process namespaces (confinement over
  names, §2.3). User files sit under all three; the on-disk uid becomes
  meaningful only when a second user exists.
- **"You start in your home directory" is POLICY, not kernel magic:**
  spawn passes cwd explicitly, so the session spawner (init/home)
  passes `/data/users/teo`. The kernel never learns what a home
  directory is. This is the explicit-at-spawn philosophy paying rent.

---

## 6. The tree — derived

Every line carries the decision it falls out of. D1 = §2 (namespace),
D2 = §3 (sealed/mutable), D3 = §4 (regions/compat), D4 = §5 (users).

```
/
├── system/                sealed region (D2). One volume today; its own
│   │                      sealed EMBKFS volume when signing lands, mount
│   │                      landing exactly here — zero path changes (§3.4).
│   ├── bin/               the system's own programs (D2 §3.1, D3 §4.1)
│   │   ├── init.elf       root namespace holder (D1 §2.4) — TO BE WRITTEN;
│   │   │                  kernel's direct spawn of home.elf is the placeholder
│   │   ├── shell.elf      the native structured shell
│   │   └── home.elf       the session UI (the compositor itself is in-kernel
│   │                      today — kernel/gfx/ — reached via its rendezvous name)
│   ├── lib/
│   │   └── libembk.so     THE hardwired loader path (elf.c) — migration item 2
│   └── abi/               the targeting contract (D2 §3.1): what it means
│       ├── include/       to compile FOR EmbLinkOS. Compilers read here,
│       ├── crt0.o         read-only; they live in /data/apps (D2 §3.2).
│       └── syscalls.o
│
├── data/                  mutable state (D2). Everything normal operation touches.
│   ├── apps/              one directory per installed application (D3 §4.1);
│   │   ├── tcc/           upgrade = replace one directory (§3.3). Compilers
│   │   ├── python/        are apps: they target /system/abi, they are not it.
│   │   ├── git/
│   │   └── …/             (sysinfo, tally, and future sval tools land here too)
│   ├── users/
│   │   └── teo/           a person's world (D4). Session cwd by init/home policy.
│   └── tmp/               scratch; where /t.o goes to die (D3 §4.1)
│
├── run/                   ephemeral epfs, UNCHANGED (audit §1.1; D1 §2.5).
│                          Rendezvous names; migrates toward a typed /services
│                          region when resolve() exists (§2.4–2.5).
│
├── fat32/                 existing FAT32 secondary volume, UNMOVED. Foreign
│                          volumes are outside the seal by nature; a proper
│                          volumes region (/data/vol/?) is deferred until
│                          removable media is a real story. Low stakes, named
│                          here so it is a decision and not an accident.
│
├── dev/null               a special NAME, not a directory (audit §1.2, §2.5):
│                          matched before VFS resolution until it resolves
│                          honestly as a typed null device.
│
└── (bin/ usr/ tmp/)       DO NOT EXIST. Reserved compat view (D3 §4.3),
                           created as symlink views only when foreign
                           software demands the names it hardcodes.
```

Depth check: the deepest real path (`/system/abi/include/…`) sits far
inside SYSCALL_PATH_MAX=256 and the shell's 128-byte spawn buffer.

### 6.1 Migration checklist (one mechanical commit, per §4.4)

**MIGRATED 2026-07-18.** The tree is live on the boot image; the mkfs
directory-support prerequisite (fe19c9d) made it mechanical. Verified: boot to
home (loader resolves /system/lib/libembk.so, kernel spawns /system/bin/home.elf),
`test layout` 9/9 (tree present, demos+fonts deliberately left at root, old flat
paths gone), `test tcc link` exit=42 (ABI at /system/abi, scratch at /data/tmp),
`test extern` OK (bare `sysinfo` resolves via the /data/apps fallback), `test
python` exit=0 (relative ._pth zip resolves after the move). SCOPE NOTE: the EmUI
demo apps and fonts stayed at root (checklist named only the core + tools);
migrating them is an additive follow-up.


Derived by diffing this tree against audit §1.2 — every hardcoded path,
accounted for:

1. **Image build** (Makefile embkfs.img packing): create the tree; place
   shell/home/init at /system/bin, libembk.so at /system/lib, headers +
   crt0.o + syscalls.o at /system/abi, tcc/python/git + sval tools into
   /data/apps/<name>/, mkdir /data/users/teo and /data/tmp.
2. **elf.c:307**: "/libembk.so" → "/system/lib/libembk.so".
3. **Boot spawn**: main.c:551/561 "/home.elf" → "/system/bin/home.elf";
   usermode.c INIT_PROGRAM_PATH likewise when init lands.
4. **eval_extern.c:92**: bare-name rule "/%s.elf" → ordered search:
   /system/bin/%s.elf, then /data/apps/%s/%s.elf. Absolute paths as-is.
5. **Selftests**: /shell.elf /tcc.elf /git.elf → their new homes;
   /t.o and scratch → /data/tmp/; /crt0.o /syscalls.o → /system/abi/.
6. **Untouched, deliberately**: /run/*, /dev/null, /fat32.
7. **Selftest for the layout itself**: assert the tree exists and the
   loader resolves /system/lib/libembk.so — a change is not done until
   a selftest exercises the invariant.

---

**This document is complete.** Reopen conditions are local to each
decision (§2.6, §3.5, §4.5); the tree changes only when a decision does.
