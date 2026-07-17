# The EmbLink Shell — Structured Pipelines, Native to the OS

**Status:** v1 shipped and live-verified (`make shell-test` = 38 host checks;
`test shell` at the kernel debug console = expression eval + a real
`ls / | where … | sort-by … | select …` pipeline over EMBKFS + the error
path, all on smp=4).

The EmbLink shell is **not** a POSIX shell. It is a structured-pipeline
shell in the Nushell/PowerShell lineage, built on EmbLink's own primitives:
spawn-with-file-actions (no fork), typed obj-handles, and kernel pipes.
Stages exchange **typed values** (tables, records, filesizes, dates), not
byte soup — `ls | where size > 1mb` compares *sizes*, it never parses
columns out of text.

```
"ls / | where size > 100kb | sort-by size | select name size"
   │
   ▼  LEXER    (shell/lex)     bytes -> tokens; 1mb pre-parsed to bytes here
   ▼  PARSER   (shell/parse)   tokens -> AST (Pratt expressions + pipeline)
   ▼  EVALUATOR(shell/eval)    AST -> a materialized Value threaded stage->stage
   ▼  SINK     (shell/main.c)  fd 3 is a pipe? emit wire frame : pretty-print fd 1

live output:
   name        size
   ----------  --------
   libembk.so  203.4 KB
   ...
   font.ttf    741.9 KB
```

---

## 1. Layout and build

```
shell/
  value/     the Value type: null/int/float/bool/string/list/record/table
             + filesize/date/path/error. VALUE SEMANTICS: every value owns
             its data, operations clone, free is a recursive walk. No
             refcounting, deliberately (shell-scale data; zero aliasing bugs).
  wire/      Value <-> bytes. Frames: [uvarint len][TLV payload]; zigzag
             varints for signed ints, LE doubles, incremental frame reader
             that survives pipe read() boundaries. Tags are ABI: append,
             never renumber.
  sval/      the SDK any program uses (the shell, ls-like tools, the future
             terminal): structured-mode detection, emit, blocking/pump
             readers, and the human renderer (aligned table grid, "1.5 MB",
             ISO-ish dates).
  lex/       hand-written scanner. Filesize literals (1mb/512kb, 1024-based)
             lexed directly; keywords and/or/not/true/false/let; both quote
             styles ("escapes", 'raw'); # comments.
  parse/     Pratt expression parser + the pipeline/command grammar.
  eval/      scope, expression evaluator (the ONE coercion site), pipeline
             runner; eval_extern.c spawns external stages (target-only).
  builtins/  ls (OS-backed) + echo/where/select/sort-by/first/count (pure).
  main.c     the REPL / -c driver (shell.elf).
  test/      host tests (make shell-test) + host stubs for the OS pieces.
```

- **Includes:** kernel convention — one `-Ishell` root, qualified paths
  (`"value/value.h"`), never ui/'s per-subdir `-I` sprawl.
- **Build:** `make build/shell.elf` (static newlib, hello.elf's link shape —
  no UI dependency). Packed automatically into `embkfs.img` (which grew
  4 MB → 8 MB to fit it).
- **Test:** `make shell-test` (host: lexer/wire/parser/eval/builtins, OS
  pieces stubbed) then `test shell` at the kernel serial console (live:
  spawn, console fds, readdir/stat, exit codes).

## 2. Running it

- **Interactive:** `run /shell.elf` at the kernel debug console (or a future
  home tile). The console delivers raw unechoed keystrokes; the shell echoes
  and handles backspace itself. `exit` leaves. Prompt: `embk>`.
- **One-shot:** `shell.elf -c "line"` — exit 0 on success, 1 on any error.
  This is what `test shell` and scripts use.
- `let` bindings persist across REPL lines (`let cutoff = 1mb`, then
  `ls | where size > $cutoff`).

## 3. The grammar

```
line     := "let" IDENT "=" expr | pipeline
pipeline := command ("|" command)*
command  := IDENT expr*            (args end at '|', ')' or end of line)
```

**Every command argument is an expression** — no command has a bespoke arg
parser. `where`'s arg is the expr `size > 1mb`; `ls`'s arg is the bare word
`/data`. Precedence (lowest → highest, left-assoc unless noted):

| level | operators | notes |
|------:|-----------|-------|
| 1 | `or` | |
| 2 | `and` | |
| 3 | `== != < > <= >= =~` | **non-chaining**: `a < b < c` is a parse error |
| 4 | `+ -` | |
| 5 | `* /` | |
| 6 | `not`, unary `-` | prefix |
| 7 | `.` field access, `( )` | postfix / grouping |

**Word rules (the gotchas, all deliberate):**
- `-` and `.` join a bare word only when flanked by word chars: `sort-by`
  and `file.txt` are one word; write **spaces** for infix (`size - 1`).
- `/` inside a word is a path (`/data/logs` is one word). A **standalone**
  `/` is division in infix position and the root path in prefix position
  (`ls /` works; `6 / 2` works; `6/2` is the word "6/2" — use spaces).
- `$var` never dot-joins: `$row.size` is field access on `$row`.

## 4. Settled decisions (were open in the sketch)

1. **Truthiness is STRICT.** In a boolean context (where's condition,
   and/or/not): Bool is itself, Null is false, anything else is an
   **error** naming the type. `where size` doesn't guess — it says
   "condition must be a boolean".
2. **Errors are values.** `VAL_ERROR` threads through the pipeline; the
   first one aborts the remaining stages; the driver prints `error: …` and
   keeps the prompt. No signals, no longjmp, no separate channel. In `-c`
   mode an error is exit code 1. Errors are never serialized (a frame
   carrying one is a shell bug; the wire layer rejects it).
3. **`sort-by <expr>`** (a key expression evaluated per row), not a bare
   `sort`. Ascending, stable (insertion sort — shell-scale data).
4. **Null propagation, SQL-style** (forced by real data: a directory's size
   is null): any `< > <= >=` against null is **false** — so
   `ls / | where size > 100kb` silently skips directories rather than
   aborting — while `sort-by` gives nulls a total order, **first**.
   `==`/`!=` treat null as equal only to null.
5. **Bare words fall back to strings.** An unresolved bare ident
   (EXPR_COLUMN) evaluates to its own name — `echo hi` and `ls /data` need
   no quotes; inside `where`, real columns win. `$vars` are the strict
   spelling: unresolved `$x` is an error.

## 5. Coercion (lives in eval, nowhere else)

The value layer refuses coercion (strict equality/compare); `expr_eval` is
the one visible place it happens:
- **Numeric family** INT / FILESIZE / DATE / FLOAT: mutually comparable
  (`size > 1000000` works when size is a filesize). FLOAT contaminates to
  double; otherwise int64.
- **Arithmetic keeps the richer tag:** `1mb + 512kb` is a filesize;
  `int + filesize` is a filesize; `date + int` is a date; `filesize + date`
  is an error; division by zero is an error.
- **Text family** STRING / PATH: mutually comparable bytes; `=~` is
  substring match (regex is future work).

## 6. The fd convention and the wire

- **fd 0** structured input (when it's a pipe), **fd 1** human text,
  **fd 3** structured output. *fd 3 being a pipe IS the signal* — no flag,
  no env var. A program checks with `sval_structured_out()` (fstat →
  S_ISFIFO; the newlib fstat/isatty stubs report the fd's TRUE backing:
  console = chardev/tty, pipe = FIFO/not-a-tty).
- A tool's shape:
  `if (sval_structured_out()) sval_emit(&t); else sval_print(&t, 1);` —
  and it may do BOTH (pretty table for the human on 1, frames on 3).
- **Wire format:** one value = one frame `[uvarint len][u8 tag][body]`;
  lengths/counts unsigned LEB128; INT/FILESIZE/DATE zigzag varints; FLOAT
  fixed 8B LE; strings length-prefixed bytes; records `{count,(name,val)*}`;
  tables rows of record bodies. 16 MB frame cap (hostile-length guard).
  Round-trip and byte-at-a-time reassembly are host-tested.

## 7. External programs in a pipeline

A stage whose name isn't a builtin spawns `/name.elf` (or an absolute path
as-is) with the Piece-0 kernel plumbing:
- a pipe's write end INSTALL_OBJ'd as the child's **fd 3**;
- if the stage has input, a second pipe's read end as the child's **fd 0**,
  the serialized input written in by the shell, then closed (EOF);
- the shell self-installs its read end (`SYS_fd_install_obj`), collects
  frames until EOF (0 → null, 1 → the value, n → a list), and reaps the
  child. COPY semantics throughout: the shell closes its own handle copies
  or nobody ever sees EOF.
- args are evaluated then passed as text argv; a child that exits non-zero
  with no structured output becomes an error value.

*Known limitation (materialize v1):* the shell writes all input before
reading output. A streaming child + input larger than the pipe buffer could
deadlock; materialize-shaped tools (read all, then emit) cannot. Streaming
stages are v2.

**PROVEN LIVE by two reference tools** (shell/tools/, packed on the image,
`test extern` = 4 assertions green):
- **`sysinfo.elf`** — the producer shape: emits one `{os, processes,
  kthreads, blocked, uptime_s}` record on fd 3 when piped, pretty-prints
  when not. `sysinfo | get processes` works like any builtin stage.
- **`tally.elf`** — the consumer shape: reads every frame from fd 0 to EOF,
  emits `{rows, frames}`. Deliberately an external re-implementation of
  `count`, so `ls | tally` cross-checks against `ls | count` — same data
  through the whole spawn/serialize/deserialize machinery.

Writing your own tool is exactly this pattern: include `sval/sval.h`, link
the SDK objects (see the Makefile's SHELL_SDK_OBJS rules), and branch on
`sval_structured_in/out()`. Drop the .elf on the image; the shell finds it
by name.

**Bug this proof caught (kernel, fixed):** a process's fds were released at
REAP time, so a dead external's fd-3 write end kept the output pipe's
writer count up until someone wait()ed it — but the shell drains to EOF
BEFORE waiting → deadlock on the very first external ever spawned. Fds are
now released at EXIT (thread_zombie_locked; Unix semantics: a zombie holds
no fds), with the reap-loop release kept as belt-and-suspenders. Also
caught: the PS/2 driver had NO shift layer — a pipeline shell whose
keyboard couldn't type `|`. Both fixed and re-verified.

## 8. Builtins — the standard names, native structured implementations

Every command with a familiar Unix name is OUR implementation: it emits
**typed values** (tables/records), never text to be re-parsed, and runs on
EmbLink's own syscalls. `help` prints this table live.

**Files & directories** (all paths resolve through the shell's own cwd —
`cd`/`pwd` are shell-side; the kernel stays absolute-paths-only):

| builtin | does |
|---|---|
| `ls [path]` | Table `{name, type, size, modified}` — modified is a real DATE (`where modified > ...` works); dirs get null size |
| `cat <file>` | contents as a String (pipe to `wc`, `=~`, `save`) |
| `cd [dir]` / `pwd` | change / show the working directory |
| `mkdir` / `rmdir` / `touch` / `rm` | create dir / remove EMPTY dir (SYS 57) / empty file / remove file (SYS 53/54) |
| `cp <src> <dst>` / `mv` | byte copy / copy+remove — O_TRUNC-clean overwrites |
| `save <path>` | write the piped value to a file (truncates: no stale tails) |

**Processes & system:**

| builtin | does |
|---|---|
| `ps` | Table `{pid, ppid, state, pri, kind}` (SYS_proc_list 55) |
| `kill <pid>` | terminate by pid (SYS_proc_kill 56 — the single-user ambient authority; unlike handle-scoped SYS_kill) |
| `env` | Table `{name, value}` — the environment handed to spawned commands |
| `env set K V` / `env unset K` | edit it (`setenv`/`unsetenv`; K may not contain `=`) |

**`env` is not cosmetic — it is the exact vector external commands receive.**
EmbLink inherits nothing: a child gets an environment only because a parent
passes one (kernel `spawn.h`), and `eval_extern.c` passes this shell's `environ`
explicitly, alongside the pipe file-actions. The shell itself starts with
whatever its own spawner named — usually nothing — so `env` on a fresh shell
being empty is correct, not a bug. `env set` then reaches the same `environ`,
because builtins run in-process (a `|` pipeline shares one).
| `uptime` / `date` | Record {seconds, pretty} / a DATE value from the RTC |
| `clear` | form-feed; the terminal wipes on `\f` |

**Pipeline stages (pure):**

| builtin | does |
|---|---|
| `echo expr*` | evaluates args: 0 → null, 1 → the value, n → a list |
| `where <expr>` | keeps rows where the expr is true (strict booleans, null = no) |
| `select col…` | projects columns in order; missing field → null cell (max 16) |
| `sort-by <expr>` | ascending by key, stable, nulls first |
| `first`/`head [n]`, `last`/`tail [n]` | first / last n rows (default 1) |
| `reverse` | rows in reverse order |
| `get <field>` | a table column as a List / a record's field |
| `count` | rows / items / 1 for a scalar / 0 for null |
| `wc` | text → {lines, words, bytes}; table → {rows} |
| `history` | past commands as a Table `{n, command}` (Up/Down recalls them) |
| `help` | the command reference, as a table |

Live-proven combos: `ps | where kind == "process"`,
`cd / | echo "alpha beta" | save f.txt | cat /f.txt | wc | get words | rm f.txt`
(the `test shell` batch), `cat notes.txt | wc`, `ls | sort-by size | last 3`.

## 9. Kernel surface the shell stands on (Piece 0)

| syscall | # | |
|---|---|---|
| `SYS_pipe` | 49 | returns two pipe-end **obj-handles** (not fds) |
| `SYS_handle_close` | 50 | drop a handle (generic kind dispatch) — what delivers EOF |
| `SYS_fd_install_obj` | 51 | install a pipe-end handle into the CALLER's own fd table |
| `SPAWN_ACTION_INSTALL_OBJ` | =4 | install a parent's pipe end as a child fd (COPY) |

Plus the fd layer (fds 0/1/2 are real console/pipe-backed slots; per-backing
ops with `close`/`close_locked`; exit-time fd release via the reap loop +
kworker) — all verified by `test pipe` (two-process EOF refcounting) and
`test fd`.

## 10. The Terminal (term.elf) — the shell on screen

The "Terminal" tile on home (or `run /term.elf`) opens an EmUI window
hosting the shell. Live-verified: typing `ls` renders the table in the
window.

It is a deliberately **dumb byte terminal** (user/bin/term.c): it spawns
`/shell.elf` with fds 0/1/2 piped (INSTALL_OBJ both ways + fd_install_obj
for its own ends), forwards keystrokes into the shell's stdin, and renders
whatever comes back — the shell already does echo, backspace erase and the
prompt, so the terminal implements no line editing. Three pieces make it
work:

- **`SYS_fd_avail` (52)** — bytes readable *right now* (pipe: buffered
  count; console: key waiting; 0 = nothing yet, NOT EOF). A render loop
  polls instead of blocking in read(). Per-backing `fd_ops.avail` hook.
- **em_app runtime hooks (V8)** — `em_set_key_hook` (raw keys before the
  toolkit; the terminal consumes everything but ESC) and `em_set_idle_hook`
  (runs every loop iteration; drains the pipe + `em_request_frame()`).
- **Hang-up for free** — closing the window drops the terminal's pipe fds
  (exit-time reap loop) → the shell's `read(0)` returns EOF → its REPL
  exits. No signals needed; the plumbing itself hangs up.

**Monospace + scrollback:** the terminal loads `/mono.ttf` (DejaVu Sans
Mono, packed by mkfs) via `EmApp.font`, so the shell's space-aligned tables
line up (verified: the `ps` grid). It keeps a 200-line scrollback ring;
**PgUp/PgDn** page it (the kernel delivers those as `EMBK_KEY_PGUP/PGDN`
= 0x0E/0x0F — extended PS/2 0x49/0x51), and typing snaps back to live.

**Command history lives in the SHELL, not the terminal** — the same split
Linux has (readline is bash's, not xterm's): the shell owns the line buffer
and does its own echo, so it is the only thing that can erase a line and
re-draw a recalled one. That gives a clean key split:

| keys | owner | does |
|---|---|---|
| **Up / Down** | shell | walk history; the line is erased (`"\b \b"` per char) and the recalled one echoed |
| **PgUp / PgDn** | terminal | scrollback paging |

The terminal simply stops swallowing Up/Down (`EMBK_KEY_UP/DOWN` =
0x13/0x14) and forwards them as plain bytes. **No escape-sequence state
machine anywhere**: the kernel delivers arrows as its own single-byte codes,
not ANSI — so recall is a byte compare, and it works identically whether
fd 0 is the Terminal's pipe or the kernel console's keyboard.

`shell/hist/` is the ring (32 entries × 512 B, fixed slots — no allocation,
oldest falls off; empty lines and adjacent duplicates are never stored,
bash's `ignoredups`). Down past the newest returns to an empty line — bash
restores the partially-typed line; we don't stash it (documented
simplification). **`history`** lists it as a Table `{n, command}`, so
`history | where command =~ "ls"` works — the payoff of never printing text.

*Terminal gotcha, learned the hard way:* the EmUI container macros
(`VStack`, `HStack`, …) are brace-scoped **for-loops**, so a `continue` or
`break` inside one targets the MACRO's hidden loop, not your own — it
silently skipped the whole render and produced a blank terminal. Use
if/else inside container scopes.

## 11. The small kernel batch (shipped)

Three gaps closed in one pass, all `test fd`-asserted live:
- **`O_TRUNC` is live** — open() shrinks to 0 through the per-fs `truncate`
  op (EMBKFS: `embkfs_truncate_object`, a real COW transaction). Both open
  paths (`vfs_open` + `fd_open_into`) enforce it; a fs without the op fails
  `-ENOSYS` loudly. `save`/`cp` overwrite cleanly now — the stale-tail trap
  is closed.
- **`rmdir`** — SYS 57 over the new per-fs `rmdir` op (EMBKFS's
  `embkfs_rmdir_name` enforces emptiness; never recursive). The shell's
  `rmdir` builtin; `mkdir x | rmdir x` is in the `test shell` batch.
- **`mtime` in `vfs_stat`** — EMBKFS's ns-resolution inode timestamps
  surfaced as seconds; syscall layer zeroes the struct so FAT32/epfs report
  an honest 0. Mirrored through `embk_vfs_stat`/`embk_stat`/newlib
  `st_mtime` (ABI: all three grow together, apps rebuilt). `ls` gained a
  `modified` DATE column.

## 12. What's deliberately NOT here (roadmap)

- **Streaming stages** (`tail -f` shapes) — v1 materializes.
- **Regex `=~`** — substring today.
- **cwd / relative paths, env vars, Ctrl-C** — tracked in the libc/OS
  roadmap (see the C++/Python/git plan), not shell-local.
- **dup2-style fd aliasing** — named gap from Piece 0; nothing needs it yet.
