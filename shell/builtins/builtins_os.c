/* ==========================================================================
 * builtins_os.c -- the OS-backed builtins: the standard command NAMES, each
 * a NATIVE structured implementation on EmbLink's own syscalls (nothing
 * shells out, nothing parses text):
 *
 *   ls cat cd pwd mkdir touch rm cp mv save ps kill uptime date clear
 *
 * TARGET-ONLY (embk syscalls); the host test build's builtin_lookup_os stub
 * returns NULL for all of these.
 *
 * The WORKING DIRECTORY is a SHELL-side concept: the kernel only speaks
 * absolute paths, so `cd` just rewrites g_cwd and every path argument is
 * resolved through path_resolve() (join + "."/".." normalization) before it
 * reaches a syscall. That keeps the kernel's path model untouched.
 * ========================================================================== */
#include "builtins/builtins.h"
#include "sval/sval.h"
#include "embk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define LS_MAX_ENTRIES 128
#define PATH_MAX_LEN   192

static char g_cwd[PATH_MAX_LEN] = "/";

/* -------------------------------------------------------------------------
 * Path resolution: absolute passes through, relative joins g_cwd; then the
 * result is normalized segment-by-segment ("." dropped, ".." pops). The
 * output is always absolute, never ends in '/' except for the root itself.
 * ------------------------------------------------------------------------- */
static void path_resolve(const char *in, char *out, size_t cap) {
    char raw[PATH_MAX_LEN * 2];
    if (in && in[0] == '/')
        snprintf(raw, sizeof raw, "%s", in);
    else
        snprintf(raw, sizeof raw, "%s/%s", g_cwd, in ? in : "");

    /* normalize into out */
    size_t o = 0;
    out[o++] = '/';
    const char *p = raw;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        size_t n = 0;
        while (p[n] && p[n] != '/') n++;
        p += n;

        if (n == 1 && seg[0] == '.') continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 1 && out[o - 1] != '/') o--;   /* pop the last segment */
            if (o > 1) o--;                            /* and its slash */
            continue;
        }
        if (o > 1 && o < cap - 1) out[o++] = '/';
        else if (o == 1) { /* first segment sits right after the root '/' */ }
        for (size_t i = 0; i < n && o < cap - 1; i++) out[o++] = seg[i];
    }
    out[o] = '\0';
}

/* One resolved path argument (arg `idx`, or the default when absent). */
static const struct value *arg_err = NULL;   /* unused sentinel */
static int arg_path(const struct command *cmd, size_t idx, const char *dflt,
                    char *out, size_t cap) {
    (void)arg_err;
    const char *word = dflt;
    if (cmd->nargs > idx) {
        word = expr_as_word(cmd->args[idx]);
        if (!word) return -1;
    }
    if (!word) return -1;
    path_resolve(word, out, cap);
    return 0;
}

static struct value err_os(const char *what, const char *path, int rc) {
    char msg[128];
    snprintf(msg, sizeof msg, "%s: '%s' (err %d)", what, path, -rc);
    return value_error(msg);
}

/* -------------------------------------------------------------------------
 * ls [path] -> Table {name, type, size}
 * ------------------------------------------------------------------------- */
static const char *dt_name(uint8_t t) {
    switch (t) {
    case EMBK_DT_REG: return "file";
    case EMBK_DT_DIR: return "dir";
    case EMBK_DT_LNK: return "link";
    default:          return "?";
    }
}

static struct value bi_ls(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    value_free(&input);

    char path[PATH_MAX_LEN];
    if (arg_path(cmd, 0, g_cwd, path, sizeof path) != 0)
        return value_error("ls: path must be a plain word or string");

    struct embk_dirent *ents =
        (struct embk_dirent *)malloc(LS_MAX_ENTRIES * sizeof(*ents));
    if (!ents) return value_error("out of memory");

    int64_t n = embk_readdir(path, ents, LS_MAX_ENTRIES);
    if (n < 0) { free(ents); return err_os("ls: can't read", path, (int)n); }

    struct value out = value_table();
    for (int64_t i = 0; i < n; i++) {
        struct value row = value_record();
        value_record_set(&row, "name", value_string(ents[i].name));
        value_record_set(&row, "type", value_string(dt_name(ents[i].type)));

        char full[PATH_MAX_LEN + 64];
        size_t plen = strlen(path);
        bool slash = plen > 0 && path[plen - 1] == '/';
        snprintf(full, sizeof full, "%s%s%s", path, slash ? "" : "/", ents[i].name);
        struct embk_stat st;
        bool have_st = (ents[i].type == EMBK_DT_REG || ents[i].type == EMBK_DT_DIR) &&
                       embk_stat(full, &st) == 0;

        if (have_st && ents[i].type == EMBK_DT_REG)
            value_record_set(&row, "size", value_filesize((int64_t)st.size));
        else
            value_record_set(&row, "size", value_null());

        /* modified: a real DATE value -- `where modified > ...` compares
         * numerically, the renderer prints it ISO-ish. 0 = fs untracked. */
        if (have_st && st.mtime > 0)
            value_record_set(&row, "modified", value_date((int64_t)st.mtime));
        else
            value_record_set(&row, "modified", value_null());

        value_table_push_row(&out, row);
    }
    free(ents);
    return out;
}

/* -------------------------------------------------------------------------
 * cd [dir] / pwd -- the shell-side working directory.
 * ------------------------------------------------------------------------- */
static struct value bi_cd(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    value_free(&input);
    char path[PATH_MAX_LEN];
    if (arg_path(cmd, 0, "/", path, sizeof path) != 0)
        return value_error("cd: path must be a plain word");
    struct embk_stat st;
    int rc = embk_stat(path, &st);
    if (rc != 0) return err_os("cd: no such directory", path, rc);
    if (st.type != EMBK_DT_DIR) return err_os("cd: not a directory", path, 0);
    snprintf(g_cwd, sizeof g_cwd, "%s", path);
    return value_null();
}

static struct value bi_pwd(const struct command *cmd, struct value input,
                           struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    return value_path(g_cwd);
}

/* -------------------------------------------------------------------------
 * cat <file> -> the contents, as a String value (pipe it to wc, =~, save).
 * ------------------------------------------------------------------------- */
static struct value bi_cat(const struct command *cmd, struct value input,
                           struct scope *env) {
    (void)env;
    value_free(&input);
    char path[PATH_MAX_LEN];
    if (cmd->nargs < 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0)
        return value_error("cat: takes a file path");

    int fd = (int)embk_open(path, 0 /* O_RDONLY */, 0);
    if (fd < 0) return err_os("cat: can't open", path, fd);

    size_t cap = 4096, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { embk_close(fd); return value_error("out of memory"); }
    for (;;) {
        if (n + 1024 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); embk_close(fd); return value_error("out of memory"); }
            buf = nb;
        }
        int64_t got = embk_read(fd, buf + n, 1024);
        if (got <= 0) break;
        n += (size_t)got;
    }
    embk_close(fd);
    struct value out = value_string_n(buf, n);
    free(buf);
    return out;
}

/* -------------------------------------------------------------------------
 * mkdir / touch / rm
 * ------------------------------------------------------------------------- */
static struct value bi_mkdir(const struct command *cmd, struct value input,
                             struct scope *env) {
    (void)env;
    value_free(&input);
    char path[PATH_MAX_LEN];
    if (cmd->nargs < 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0)
        return value_error("mkdir: takes a directory path");
    int rc = embk_mkdir(path);
    if (rc != 0) return err_os("mkdir: can't create", path, rc);
    return value_null();
}

static struct value bi_touch(const struct command *cmd, struct value input,
                             struct scope *env) {
    (void)env;
    value_free(&input);
    char path[PATH_MAX_LEN];
    if (cmd->nargs < 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0)
        return value_error("touch: takes a file path");
    int fd = (int)embk_open(path, EMBK_O_CREAT | EMBK_O_WRONLY, 0644);
    if (fd < 0) return err_os("touch: can't create", path, fd);
    embk_close(fd);
    return value_null();
}

static struct value bi_rm(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    value_free(&input);
    char path[PATH_MAX_LEN];
    if (cmd->nargs < 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0)
        return value_error("rm: takes a file path");
    int rc = embk_unlink(path);
    if (rc != 0) return err_os("rm: can't remove", path, rc);
    return value_null();
}

static struct value bi_rmdir(const struct command *cmd, struct value input,
                             struct scope *env) {
    (void)env;
    value_free(&input);
    char path[PATH_MAX_LEN];
    if (cmd->nargs < 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0)
        return value_error("rmdir: takes a directory path");
    int rc = embk_rmdir(path);
    if (rc != 0) return err_os("rmdir: can't remove (not empty?)", path, rc);
    return value_null();
}

/* -------------------------------------------------------------------------
 * cp <src> <dst> / mv <src> <dst>. Byte copy through a bounce buffer.
 * (No O_TRUNC in the kernel yet: overwriting a LONGER existing file leaves
 * its tail -- copy to fresh names; documented limitation.)
 * ------------------------------------------------------------------------- */
static struct value copy_file(const char *verb, const char *src, const char *dst) {
    int in = (int)embk_open(src, 0, 0);
    if (in < 0) return err_os(verb, src, in);
    int out = (int)embk_open(dst, EMBK_O_CREAT | EMBK_O_WRONLY | EMBK_O_TRUNC, 0644);
    if (out < 0) { embk_close(in); return err_os(verb, dst, out); }

    char buf[1024];
    for (;;) {
        int64_t got = embk_read(in, buf, sizeof buf);
        if (got < 0) { embk_close(in); embk_close(out); return err_os(verb, src, (int)got); }
        if (got == 0) break;
        int64_t off = 0;
        while (off < got) {
            int64_t w = embk_write(out, buf + off, (size_t)(got - off));
            if (w <= 0) { embk_close(in); embk_close(out); return err_os(verb, dst, (int)w); }
            off += w;
        }
    }
    embk_close(in);
    embk_close(out);
    return value_null();
}

static struct value bi_cp(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    value_free(&input);
    char src[PATH_MAX_LEN], dst[PATH_MAX_LEN];
    if (cmd->nargs < 2 ||
        arg_path(cmd, 0, NULL, src, sizeof src) != 0 ||
        arg_path(cmd, 1, NULL, dst, sizeof dst) != 0)
        return value_error("cp: takes <src> <dst>");
    return copy_file("cp", src, dst);
}

static struct value bi_mv(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    value_free(&input);
    char src[PATH_MAX_LEN], dst[PATH_MAX_LEN];
    if (cmd->nargs < 2 ||
        arg_path(cmd, 0, NULL, src, sizeof src) != 0 ||
        arg_path(cmd, 1, NULL, dst, sizeof dst) != 0)
        return value_error("mv: takes <src> <dst>");
    struct value rc = copy_file("mv", src, dst);
    if (rc.type == VAL_ERROR) return rc;
    int u = embk_unlink(src);
    if (u != 0) return err_os("mv: copied, but can't remove", src, u);
    return value_null();
}

/* -------------------------------------------------------------------------
 * save <path> -- write the PIPED value to a file, rendered as text
 * (`ls | save /listing.txt`, `cat a.txt | save /b.txt`).
 * ------------------------------------------------------------------------- */
static struct value bi_save(const struct command *cmd, struct value input,
                            struct scope *env) {
    (void)env;
    char path[PATH_MAX_LEN];
    if (cmd->nargs < 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0) {
        value_free(&input);
        return value_error("save: takes a destination path");
    }
    if (input.type == VAL_NULL) return value_error("save: nothing to save (pipe a value in)");

    int fd = (int)embk_open(path, EMBK_O_CREAT | EMBK_O_WRONLY | EMBK_O_TRUNC, 0644);
    if (fd < 0) { value_free(&input); return err_os("save: can't create", path, fd); }
    int rc = sval_print(&input, fd);
    embk_close(fd);
    value_free(&input);
    if (rc != 0) return err_os("save: write failed", path, rc);
    return value_null();
}

/* -------------------------------------------------------------------------
 * ps -> Table {pid, ppid, state, pri, kind}   |   kill <pid>
 * ------------------------------------------------------------------------- */
static struct value bi_ps(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    static const char *state_names[] = { "unused", "ready", "running", "blocked", "zombie" };

    struct embk_proc_info info[64];
    int n = embk_proc_list(info, 64);
    if (n < 0) return value_error("ps: proc_list failed");

    struct value out = value_table();
    for (int i = 0; i < n; i++) {
        struct value r = value_record();
        value_record_set(&r, "pid",   value_int((int64_t)info[i].pid));
        value_record_set(&r, "ppid",  value_int((int64_t)info[i].parent_pid));
        value_record_set(&r, "state", value_string(
            (info[i].state >= 0 && info[i].state <= 4) ? state_names[info[i].state] : "?"));
        value_record_set(&r, "pri",   value_int((int64_t)info[i].priority));
        value_record_set(&r, "kind",  value_string(info[i].is_kthread ? "kthread" : "process"));
        value_table_push_row(&out, r);
    }
    return out;
}

/* -------------------------------------------------------------------------
 * env -- the environment this shell will hand to the children it spawns.
 *
 * EmbLink inherits NOTHING: a child gets an environment only because a parent
 * passes one (kernel spawn.h). eval_extern.c passes THIS -- so what `env` shows
 * is exactly what the next external command will receive, not an approximation.
 *
 *   env                 -> table {name, value}
 *   env set KEY VALUE   -> set/overwrite
 *   env unset KEY       -> remove
 * ------------------------------------------------------------------------- */
static struct value bi_env(const struct command *cmd, struct value input,
                           struct scope *env) {
    value_free(&input);

    if (cmd->nargs == 0) {
        struct value out = value_table();
        for (int i = 0; environ && environ[i]; i++) {
            const char *e = environ[i];
            const char *eq = strchr(e, '=');
            /* No '=' should be impossible, but show it rather than hide it:
             * silently dropping a row would make a broken entry invisible. */
            struct value r = value_record();
            if (!eq) {
                value_record_set(&r, "name",  value_string(e));
                value_record_set(&r, "value", value_null());
            } else {
                char name[128];
                size_t nlen = (size_t)(eq - e);
                if (nlen >= sizeof name) nlen = sizeof name - 1;
                memcpy(name, e, nlen);
                name[nlen] = '\0';
                value_record_set(&r, "name",  value_string(name));
                value_record_set(&r, "value", value_string(eq + 1));
            }
            value_table_push_row(&out, r);
        }
        return out;
    }

    /* Sub-commands take BARE WORDS, not expressions: `env set HOME /` should not
     * try to evaluate HOME as a variable. cmd->args carry the raw text. */
    const char *sub = cmd->args[0] ? expr_as_word(cmd->args[0]) : NULL;
    if (!sub) return value_error("env: usage: env | env set KEY VALUE | env unset KEY");

    if (strcmp(sub, "set") == 0) {
        if (cmd->nargs != 3) return value_error("env set: needs KEY and VALUE");
        const char *k = expr_as_word(cmd->args[1]);
        const char *v = expr_as_word(cmd->args[2]);
        if (!k || !v) return value_error("env set: KEY and VALUE must be words");
        if (strchr(k, '=')) return value_error("env set: KEY cannot contain '='");
        if (setenv(k, v, 1) != 0) return value_error("env set: out of memory");
        return value_null();
    }
    if (strcmp(sub, "unset") == 0) {
        if (cmd->nargs != 2) return value_error("env unset: needs a KEY");
        const char *k = expr_as_word(cmd->args[1]);
        if (!k) return value_error("env unset: KEY must be a word");
        /* unsetenv of an absent name succeeds -- removing what isn't there is
         * not an error, it is the requested end state. */
        if (unsetenv(k) != 0) return value_error("env unset: invalid KEY");
        return value_null();
    }
    return value_error("env: usage: env | env set KEY VALUE | env unset KEY");
}

static struct value bi_kill(const struct command *cmd, struct value input,
                            struct scope *env) {
    value_free(&input);
    if (cmd->nargs != 1) return value_error("kill: takes a pid (see ps)");
    struct value pv = expr_eval(cmd->args[0], env);
    if (pv.type == VAL_ERROR) return pv;
    if (pv.type != VAL_INT || pv.u.i <= 0) {
        value_free(&pv);
        return value_error("kill: pid must be a positive integer");
    }
    uint32_t pid = (uint32_t)pv.u.i;
    int rc = embk_proc_kill(pid);
    if (rc != 0) {
        char msg[64];
        snprintf(msg, sizeof msg, "kill: no live process with pid %u", pid);
        return value_error(msg);
    }
    return value_null();
}

/* -------------------------------------------------------------------------
 * uptime / date / clear
 * ------------------------------------------------------------------------- */
static struct value bi_uptime(const struct command *cmd, struct value input,
                              struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    uint64_t ms = embk_uptime_ms();
    uint64_t s = ms / 1000;
    char pretty[32];
    snprintf(pretty, sizeof pretty, "%lu:%02lu:%02lu",
             (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60),
             (unsigned long)(s % 60));
    struct value out = value_record();
    value_record_set(&out, "seconds", value_int((int64_t)s));
    value_record_set(&out, "pretty",  value_string(pretty));
    return out;
}

static struct value bi_date(const struct command *cmd, struct value input,
                            struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    time_t now = time(NULL);
    if (now <= 0) return value_error("date: no wall clock");
    return value_date((int64_t)now);   /* a DATE value -- renders ISO-ish,
                                        * compares numerically in `where` */
}

static struct value bi_clear(const struct command *cmd, struct value input,
                             struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    write(1, "\f", 1);   /* the terminal clears on form-feed; harmless on serial */
    return value_null();
}

/* -------------------------------------------------------------------------
 * The OS registry (builtin_lookup falls through to this).
 * ------------------------------------------------------------------------- */
builtin_fn builtin_lookup_os(const char *name) {
    static const struct { const char *name; builtin_fn fn; } tab[] = {
        { "ls",     bi_ls     }, { "cat",    bi_cat    },
        { "cd",     bi_cd     }, { "pwd",    bi_pwd    },
        { "mkdir",  bi_mkdir  }, { "rmdir",  bi_rmdir  },
        { "touch",  bi_touch  },
        { "rm",     bi_rm     }, { "cp",     bi_cp     },
        { "mv",     bi_mv     }, { "save",   bi_save   },
        { "ps",     bi_ps     }, { "kill",   bi_kill   },
        { "env",    bi_env    },
        { "uptime", bi_uptime }, { "date",   bi_date   },
        { "clear",  bi_clear  },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcmp(tab[i].name, name) == 0) return tab[i].fn;
    return NULL;
}
