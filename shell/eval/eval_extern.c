/* ==========================================================================
 * eval_extern.c -- one EXTERNAL pipeline stage. TARGET-ONLY (embk syscalls);
 * the host test build links shell/test/host_stubs.c instead.
 *
 * The plumbing (Piece 0's whole payoff):
 *   - pipe A carries the child's structured OUTPUT: its write end is
 *     INSTALL_OBJ'd as the child's fd 3; the shell reads frames from its own
 *     fd (the read end, self-installed via SYS_fd_install_obj).
 *   - pipe B (only when this stage has INPUT) carries the previous stage's
 *     serialized value INTO the child's fd 0.
 *   - COPY semantics everywhere: after the spawn, the shell MUST close its
 *     own handles to the ends it gave the child, or the child never sees
 *     EOF on stdin and the shell never sees EOF on the output pipe.
 *
 * STREAMING I/O (v2 of this file): the shell no longer writes ALL input
 * before reading any output. The pump below interleaves -- feed stdin
 * whenever the pipe has space (fd_avail on a WRITE end = free bytes), drain
 * fd-3 whenever it has data -- so a child that emits while its stdin is
 * still arriving, with values larger than the 1KB pipe buffer on BOTH
 * sides, can't deadlock the shell. Once stdin is fully fed and closed, the
 * remainder is a plain blocking drain. (Stage-to-stage streaming of ROWS
 * through builtins is still materialize-v1 -- nothing produces unbounded
 * streams yet; this closes the deadlock, not the memory model.)
 * ========================================================================== */
#include "eval/eval.h"
#include "sval/sval.h"
#include "wire/wire.h"
#include "embk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

extern void shell_tty_suspend(void);   /* main.c: cooked for the child's lifetime */
extern void shell_tty_resume(void);

/* Scratch fds for the shell's own ends of the plumbing. Above stdio+fd3,
 * below anything vfs_open would hand out mid-pipeline in practice. */
#define EXT_FD_INPUT_WRITE 10
#define EXT_FD_OUTPUT_READ 11

#define EXT_ARGV_MAX 8
#define EXT_EPIPE 32   /* -EMBK_EPIPE (errno.h is kernel-only) -- the ONE write
                        * error that means "child hung up stdin", vs a transient
                        * fault we must retry instead of truncating the frame. */

/* Fold one collected frame into the stage result: 0 frames -> that value,
 * 2+ -> a list. Shared by the pump and the tail drain. */
static void absorb_frame(struct value *collected, size_t *nframes, struct value v) {
    if (*nframes == 0) {
        *collected = v;
    } else if (*nframes == 1) {
        struct value list = value_list();
        value_list_push(&list, *collected);
        value_list_push(&list, v);
        *collected = list;
    } else {
        value_list_push(collected, v);
    }
    (*nframes)++;
}

/* Grace-then-kill: the ESCALATION half of docs/INTERRUPTION.md §4.3, and it is
 * deliberately SHELL policy, not kernel law.
 *
 *   ^C -> kernel cancels the child (polite; it may clean up and exit)
 *         child still alive GRACE_MS later -> embk_kill (uncatchable)
 *
 * The shell never sees the ^C -- the kernel consumed it -- so the clock starts
 * from OBSERVING the child's cancel state (embk_child_cancelled), which is what
 * keeps "cancelled but declining" distinguishable from "healthy but slow": a
 * slow child that was never cancelled is never killed by this.
 *
 * 1500ms: long enough for real cleanup (delete a lock file, flush a buffer),
 * short enough that a ^C visibly works. Tunable here, visibly, like any policy. */
#define SHELL_CANCEL_GRACE_MS 1500

struct cancel_watch { uint64_t seen_ms; bool killed; };

static void cancel_watch_tick(struct cancel_watch *w, int64_t child) {
    if (w->killed) return;
    if (embk_child_cancelled((int)child) != 1) return;
    uint64_t now = embk_uptime_ms();
    if (w->seen_ms == 0) { w->seen_ms = now; return; }   /* clock starts */
    if (now - w->seen_ms >= SHELL_CANCEL_GRACE_MS) {
        (void)embk_kill((int)child);   /* exit closes its fds -> our pipe EOFs */
        w->killed = true;
    }
}

struct value extern_stage_run(const struct command *cmd, struct value input,
                              struct scope *env) {
    char msg[128];

    /* --- resolve the program path (docs/USERSPACE.md D3 §4.2) ---
     * Absolute path: used as-is. Bare name: searched against an ORDERED,
     * shell-owned list -- /system/bin first, then /data/apps/<name>/ -- taking
     * the FIRST that exists. There is deliberately NO PATH env var (§4.2: that is
     * ambient authority the per-process namespace already grants); the order is
     * policy and lives HERE, in one visible place. First-match-wins means a name
     * present in both /system/bin and /data/apps resolves to /system/bin.
     * 256, not 128: /data/apps/<name>/<name>.elf spells the name TWICE, so a long
     * app name overflowed the old buffer (snprintf would truncate to a wrong,
     * silently-different path). */
    char path[256];
    struct embk_stat st;
    if (cmd->name[0] == '/') {
        snprintf(path, sizeof path, "%s", cmd->name);
    } else {
        snprintf(path, sizeof path, "/system/bin/%s.elf", cmd->name);
        if (embk_stat(path, &st) < 0)
            snprintf(path, sizeof path, "/data/apps/%s/%s.elf", cmd->name, cmd->name);
        /* If neither exists, `path` holds the /data/apps candidate and the spawn
         * below fails with an ENOENT that names it -- an honest miss, not a
         * silent wrong-file lookup. */
    }

    /* --- argv: each arg evaluated in the driver scope, formatted as text --- */
    char *argv[EXT_ARGV_MAX + 2];
    char  argbuf[EXT_ARGV_MAX][96];
    int   argc = 0;
    argv[argc++] = path;
    if (cmd->nargs > EXT_ARGV_MAX) {
        value_free(&input);
        return value_error("too many arguments for an external command");
    }
    for (size_t i = 0; i < cmd->nargs; i++) {
        struct value v = expr_eval(cmd->args[i], env);
        if (v.type == VAL_ERROR) { value_free(&input); return v; }
        sval_format_scalar(&v, argbuf[i], sizeof argbuf[i]);
        value_free(&v);
        argv[argc++] = argbuf[i];
    }
    argv[argc] = NULL;

    /* --- serialize the input up front (frees the value immediately; the
     * pump then streams these bytes at the pipe's pace) --- */
    bool has_input = input.type != VAL_NULL;
    struct wire_buf inframe;
    wire_buf_init(&inframe);
    if (has_input && wire_serialize(&input, &inframe) != 0) {
        value_free(&input);
        wire_buf_free(&inframe);
        return value_error("out of memory serializing pipeline input");
    }
    value_free(&input);

    /* --- pipe A: child structured output (child fd 3) --- */
    int pa[2];   /* {read_handle, write_handle} */
    if (embk_pipe(pa) != 0) { wire_buf_free(&inframe); return value_error("pipe failed"); }

    struct embk_spawn_file_action acts[2];
    memset(acts, 0, sizeof acts);
    int nact = 0;
    acts[nact].kind = EMBK_SPAWN_ACTION_INSTALL_OBJ;
    acts[nact].target_fd = SVAL_FD_OUT;
    acts[nact].src_obj_handle = pa[1];
    nact++;

    /* --- pipe B: structured input (child fd 0), only when there IS input --- */
    int pb[2] = { -1, -1 };
    if (has_input) {
        if (embk_pipe(pb) != 0) {
            embk_close_handle(pa[0]); embk_close_handle(pa[1]);
            wire_buf_free(&inframe);
            return value_error("pipe failed");
        }
        acts[nact].kind = EMBK_SPAWN_ACTION_INSTALL_OBJ;
        acts[nact].target_fd = SVAL_FD_IN;
        acts[nact].src_obj_handle = pb[0];
        nact++;
    }

    shell_tty_suspend();

    /* --- spawn ---
     * The child gets THE SHELL'S OWN environment, and gets it because this line
     * names it. Nothing is inherited on EmbLink: a child receives only what its
     * parent passes (see the kernel's spawn.h), so `environ` here is a decision,
     * visible at the call site, exactly like the file-actions above. Edit it
     * with the `env` builtin; inspect it with plain `env`. */
    int64_t child = embk_spawn_env(path, argv, environ, acts, nact);
    if (child >= 0) {
        /* HAND OVER the right to be interrupted for as long as this command runs
         * (docs/INTERRUPTION.md). A ^C now cancels the CHILD, not this shell.
         *
         * This is the delegation the whole design turns on: EmbLink has no
         * foreground process to infer, so the shell -- which holds the console --
         * simply says who ^C means while it is not the one running. Reclaimed
         * unconditionally after the wait below; leaving it pointing at a reaped
         * child would make the NEXT ^C silently do nothing. */
        (void)embk_console_interrupt_route((int)child);
    }
    if (child < 0) {
        embk_close_handle(pa[0]); embk_close_handle(pa[1]);
        if (has_input) { embk_close_handle(pb[0]); embk_close_handle(pb[1]); }
        wire_buf_free(&inframe);
        shell_tty_resume();   /* suspend() ran BEFORE the spawn, so undo it even
                               * when the child never came to exist */
        snprintf(msg, sizeof msg, "%s: can't run (err %d)", cmd->name, (int)-child);
        return value_error(msg);
    }

    /* --- drop OUR copies of the ends the child now holds (EOF depends on it) --- */
    embk_close_handle(pa[1]);
    if (has_input) embk_close_handle(pb[0]);

    /* --- our own ends become plain fds --- */
    if (embk_fd_install_obj(pa[0], EXT_FD_OUTPUT_READ) < 0) {
        embk_close_handle(pa[0]);
        if (has_input) { embk_close_handle(pb[1]); }
        wire_buf_free(&inframe);
        shell_tty_resume();
        embk_wait((int)child);
        return value_error("fd install failed");
    }
    embk_close_handle(pa[0]);
    bool in_open = false;
    if (has_input) {
        if (embk_fd_install_obj(pb[1], EXT_FD_INPUT_WRITE) < 0) {
            embk_close_handle(pb[1]);
            close(EXT_FD_OUTPUT_READ);   /* an FD (self-installed above), not a handle */
            wire_buf_free(&inframe);
            shell_tty_resume();
            embk_wait((int)child);
            return value_error("fd install failed");
        }
        embk_close_handle(pb[1]);
        in_open = true;
    }

    /* --- the STREAMING pump: interleave until stdin is fully fed --- */
    struct sval_reader rd;
    sval_reader_init(&rd);
    struct value collected = value_null();
    size_t nframes = 0;
    size_t off = 0;
    int rc = 0;
    struct cancel_watch cw = {0, false};

    while (in_open && rc == 0) {
        bool progress = false;

        int64_t space = embk_fd_avail(EXT_FD_INPUT_WRITE);
        if (space > 0 && off < inframe.len) {
            size_t chunk = inframe.len - off;
            if (chunk > (size_t)space) chunk = (size_t)space;
            int64_t w = embk_write(EXT_FD_INPUT_WRITE, inframe.data + off, chunk);
            if (w > 0) { off += (size_t)w; progress = true; }
            else if (w == -EXT_EPIPE) off = inframe.len;   /* child hung up stdin
                                                            * early -- legal; stop */
            /* any OTHER w <= 0: a transient fault -- leave `off` put and fall
             * through to yield + retry. Truncating here would hand the child a
             * partial frame ("corrupt structured input") -- the flaky bug this
             * replaced, which fired only under scheduler pressure. */
        }
        if (off >= inframe.len) {
            /* EOF to the child. close(), NOT embk_close_handle():
             * EXT_FD_INPUT_WRITE is an FD (self-installed via fd_install_obj;
             * its handle was closed right after installing). close_handle(10)
             * silently hit an unrelated/empty HANDLE slot, the pipe's write end
             * stayed open, the child never saw EOF, and every extern CONSUMER
             * pipeline (`ls / | tally`) wedged: child blocked on stdin forever,
             * this pump spinning in the drain. The drain's own close at the
             * bottom of this function always had it right. */
            close(EXT_FD_INPUT_WRITE);
            in_open = false;
            progress = true;
        }

        for (;;) {
            int64_t nav = embk_fd_avail(EXT_FD_OUTPUT_READ);
            if (nav <= 0) break;
            uint8_t tmp[256];
            size_t want = nav > (int64_t)sizeof tmp ? sizeof tmp : (size_t)nav;
            int64_t got = embk_read(EXT_FD_OUTPUT_READ, tmp, want);
            if (got <= 0) break;
            if (sval_reader_feed(&rd, tmp, (size_t)got) != 0) { rc = -1; break; }
            struct value v;
            int prc;
            while ((prc = sval_reader_next(&rd, &v)) == 1)
                absorb_frame(&collected, &nframes, v);
            if (prc < 0) { rc = -1; break; }
            progress = true;
        }

        if (!progress) {
            /* Both pipes momentarily stuck: the child is computing (or
             * declining a cancellation). Run the escalation policy, then give
             * it the CPU. */
            cancel_watch_tick(&cw, child);
            embk_yield();
        }
    }
    if (in_open) close(EXT_FD_INPUT_WRITE);      /* error exit mid-feed (an FD, see above) */
    wire_buf_free(&inframe);

    /* --- stdin done: drain the output to EOF --- */
    if (rc == 0) {
        /* THE FIX FOR THE ESCALATION GAP: this used to be a plain blocking
         * read loop -- for a command with no piped input the shell spent the
         * child's ENTIRE runtime here, so if a ^C'd child declined to exit,
         * no shell code ever ran again and the grace-then-kill policy was
         * unreachable. Poll instead: consume what's available, and while the
         * pipe is quiet run the policy.
         *
         * The final blocking drain below is safe BECAUSE the child is gone by
         * then: fds are released AT EXIT, not at reap ("a zombie holds no
         * fds", process.c) -- and embk_kill drives that same last-thread
         * transition -- so once proc_alive says dead, the write end is closed
         * (or closing) and read returns the trailing bytes then EOF; it
         * cannot hang. Draining even after a kill is deliberate: frames the
         * child completed before dying are real results. */
        for (;;) {
            int64_t nav = embk_fd_avail(EXT_FD_OUTPUT_READ);
            if (nav > 0) {
                uint8_t tmp[256];
                size_t want = nav > (int64_t)sizeof tmp ? sizeof tmp : (size_t)nav;
                int64_t got = embk_read(EXT_FD_OUTPUT_READ, tmp, want);
                if (got <= 0) { rc = -1; break; }
                if (sval_reader_feed(&rd, tmp, (size_t)got) != 0) { rc = -1; break; }
                struct value v;
                int prc;
                while ((prc = sval_reader_next(&rd, &v)) == 1)
                    absorb_frame(&collected, &nframes, v);
                if (prc < 0) { rc = -1; break; }
                continue;
            }
            if (!embk_proc_alive((int)child)) {
                struct value v;
                int brc;
                while ((brc = sval_read_blocking(&rd, EXT_FD_OUTPUT_READ, &v)) == 1)
                    absorb_frame(&collected, &nframes, v);
                if (brc < 0) rc = -1;
                break;
            }
            cancel_watch_tick(&cw, child);
            /* 10ms, not yield: this loop can run for the child's whole life,
             * and a yield-spin would burn a core against it. 10ms is invisible
             * next to human-scale output and the 1500ms grace. */
            embk_sleep_ms(10);
        }
    }
    sval_reader_free(&rd);
    close(EXT_FD_OUTPUT_READ);

    int64_t code = embk_wait((int)child);

    /* Take the console back: from here on ^C means THIS shell again. Done
     * unconditionally -- a route left pointing at a reaped child makes the next
     * ^C silently do nothing, which reads as a bug rather than a stale route. */
    (void)embk_console_interrupt_route(-1);

    /* Console back in the shell's hands -> its own line editing resumes. One
     * resume here covers all three returns below. Route-clear and cooked-undo
     * are the same event ("the child no longer owns the console"), after the
     * same reap -- which is why they sit together. */
    shell_tty_resume();

    if (rc < 0) {
        value_free(&collected);
        snprintf(msg, sizeof msg, "%s: corrupt structured output", cmd->name);
        return value_error(msg);
    }
    if (nframes == 0 && code != 0) {
        /* no structured output AND a failing exit: surface the failure */
        snprintf(msg, sizeof msg, "%s: exited with code %lld", cmd->name, (long long)code);
        return value_error(msg);
    }
    return collected;   /* 0 frames -> null, 1 -> the value, n -> a list */
}
