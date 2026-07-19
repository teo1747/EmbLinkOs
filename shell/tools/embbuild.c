/* EmbBuild v1 -- the typed-manifest walker (docs/BUILD.md).
 *
 * Core loop: parse stanzas -> build the output map -> ordering check ->
 * walk top to bottom, hash/compare/spawn/stamp PER STANZA IN SEQUENCE.
 * The sequencing is semantic, not stylistic: each stanza hashes its
 * inputs AS THEY EXIST when the walk reaches it, so bytes produced
 * earlier in this run flow into later stamps. That chaining is what
 * makes a mis-ordered manifest cost one extra run instead of a
 * permanently stale artifact -- and the parse-time guard promotes even
 * that one run from silent to fatal.
 *
 * The guard is a CHECKER, not a compensator: it derives no order and
 * executes nothing differently -- it obeys the author's ordering or
 * refuses a manifest that lies about itself. Same line docs/BUILD.md §3
 * draws between a build tool and `curl | sudo sh`.
 *
 * Must remain TCC-compilable (self-rebuild is target #3): C99, static
 * storage, no __thread, no dynamic linking. */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include "embk.h"

#define EMBBUILD_VERSION "embbuild v1.0.0 (2024-06-05)"  /* part of every stamp: a tool
                                                            upgrade must rebuild the world*/

/* Fixed caps, loud on overflow -- house style. Generous vs. the reel
 * graph (~50 nodes total across ALL projects). */
#define MAX_STANZAS 64
#define MAX_ITEMS   32       /* INPUTS or ARGS words per stanza */
#define MAX_TOK     256     
#define MAX_LINE    1024      

struct stanza {
    char name[MAX_TOK];
    char kind[MAX_TOK];                                     /* "compile" | "link" | "install" */
    char inputs[MAX_ITEMS][MAX_TOK]; int n_inputs;
    char argv[MAX_ITEMS][MAX_TOK]; int n_args;
    char output[MAX_TOK];
    char stamp[MAX_TOK];                                     /* the hash of the inputs, args, and tool version */
    int line;                                                /* for diagnostics */
};

static struct stanza g_stanzas[MAX_STANZAS];
static int g_n_stanzas = 0;
static char g_project[MAX_TOK];

/* ---------------- CRC32C (Castagnoli), software, table-driven ---------------- 
 * the house hash. Threat model per BUILD.md §4: ACCIDENTAL collisions only.
 * Table built at startup: 30 lines beats an SSE4.2 dependency TCC would have
 * to reproduce when it rebuilds this file. */
static uint32_t crc32c_table[256];
static void crc32c_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82f63b78;
            } else {
                crc >>= 1;
            }
        }
        crc32c_table[i] = crc;  
    }
}

static uint32_t crc_feed(uint32_t crc, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    while (len--) {
        crc = (crc >> 8) ^ crc32c_table[(crc ^ *p++) & 0xff];
    }
    return crc;
}

/* Feed one file's bytes into the running hash. Missing input = FATAL:
 * a stanza whose input doesn't exist when its turn comes is either a 
 * mis-ordered manifest (the guard catches the in-manifest case) or a
 * missing source -- both are the author's problem, stated loudly. */
static int crc_feed_file(uint32_t *crc, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "embbuild: input missing: %s\n", path);
        return -1;
    }
    unsigned char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        *crc = crc_feed(*crc, buf, (size_t)n);
    }
    close(fd);
    if (n < 0) {
        fprintf(stderr, "embbuild: read error: %s\n", path);
        return -1;
    }
    return 0;
}

/* ---------------------------- Manifest parser ---------------------------------
 * Format (BUILD.md §5): `project:` first; stanzas of `key: value` lines
 * separated by blank lines; lists whitespace-split; '#' starts a comment. */
static char *trim(char *s) {
    while (*s && (*s == ' ' || *s == '\t')) s++;
    char *end = s + strlen(s);
    while (end > s && (*(end - 1) == ' ' || *(end - 1) == '\t' || *(end - 1) == '\n' || *(end - 1) == '\r')) {
        *(--end) = 0;
    }
    return s;
}

static int split_list(char *v, char out[][MAX_TOK], int max, const char *what,
                      const char *stanza_name) {
    int count = 0;
    for (char *token = strtok(v, " \t"); token != NULL; token = strtok(NULL, " \t")) {
        if (count >= max) {
            fprintf(stderr, "embbuild: too many %s in stanza '%s'\n", what, stanza_name);
            return -1;
        }
        if (strlen(token) >= MAX_TOK) {
            fprintf(stderr, "embbuild: %s token too long in stanza '%s'\n", what, stanza_name);
            return -1;
        }
        strcpy(out[count++], token);
    }
    return count;
}

static int parse_manifest(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "embbuild: cannot open manifest: %s\n", path);
        return -1;
    }
    char line[MAX_LINE];
    int line_num = 0;
    struct stanza *current_stanza = NULL;

    while (fgets(line, sizeof(line), f)) {
        line_num++;
        char *trimmed = trim(line);
        if (*trimmed == '#' || *trimmed == 0) {
            if (*trimmed == 0) {
                current_stanza = NULL;  // Blank line resets current stanza
            }
            continue;  // Skip comments and empty lines
        }
        char *colon = strchr(trimmed, ':');
        if (!colon) {
            fprintf(stderr, "embbuild: syntax error in manifest at line %d: %s\n", line_num, trimmed);
            fclose(f);
            return -1;
        }
        *colon = 0;
        char *key = trim(trimmed), *value = trim(colon + 1);
        if (strcmp(key, "project") == 0) {
            strncpy(g_project, value, sizeof(g_project) - 1);
            continue;
        }
        if (!current_stanza) {
            if (g_n_stanzas >= MAX_STANZAS) {
                fprintf(stderr, "embbuild: too many stanzas in manifest\n");
                fclose(f);
                return -1;
            }
            current_stanza = &g_stanzas[g_n_stanzas++];
            current_stanza->line = line_num;
            current_stanza->n_inputs = 0;
            current_stanza->n_args = 0;

            memset(current_stanza->name, 0, sizeof(current_stanza->name));
        }
        if (strcmp(key, "name") == 0) strncpy(current_stanza->name, value, sizeof(current_stanza->name) - 1);
        else if (strcmp(key, "kind") == 0) strncpy(current_stanza->kind, value, sizeof(current_stanza->kind) - 1);
        else if (strcmp(key, "inputs") == 0) {
            current_stanza->n_inputs = split_list(value, current_stanza->inputs, MAX_ITEMS, "inputs", current_stanza->name);
            if (current_stanza->n_inputs < 0) { fclose(f); return -1; }
        } else if (strcmp(key, "args") == 0) {
            current_stanza->n_args = split_list(value, current_stanza->argv, MAX_ITEMS, "args", current_stanza->name);
            if (current_stanza->n_args < 0) { fclose(f); return -1; }
        } else if (strcmp(key, "output") == 0) strncpy(current_stanza->output, value, sizeof(current_stanza->output) - 1);
        else {
            fprintf(stderr, "embbuild: unknown key '%s' in stanza '%s' at line %d\n", key, current_stanza->name, line_num);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    if (!g_project[0]) {
        fprintf(stderr, "embbuild: manifest missing 'project:' declaration\n");
        return -1;
    }
    for (int i = 0; i < g_n_stanzas; i++) {
        struct stanza *s = &g_stanzas[i];
        if (!s->name[0]) {
            fprintf(stderr, "embbuild: stanza at line %d missing 'name:'\n", s->line);
            return -1;
        }
        if (!s->kind[0]) {
            fprintf(stderr, "embbuild: stanza '%s' at line %d missing 'kind:'\n", s->name, s->line);
            return -1;
        }
        if (!s->output[0]) {
            fprintf(stderr, "embbuild: stanza '%s' at line %d missing 'output:'\n", s->name, s->line);
            return -1;
        }
    }
    return 0;
}

/* ---------------------------- The Ordering Guard ---------------------------------
 * If stanza i consumes the output of stanza j, then j must precede i in the manifest, 
 * it contradicts its own ordering (j == i is self-consumption, which is also an error).
 * Refuse. One map, Zero graph theory; converging-next-run becomes fatal-now. */
static int ordering_check(void) {
    for (int i = 0; i < g_n_stanzas; i++) {
        struct stanza *si = &g_stanzas[i];
        for (int j = 0; j < g_n_stanzas; j++) {
            if (i == j) continue;
            struct stanza *sj = &g_stanzas[j];
            for (int k = 0; k < si->n_inputs; k++) {
                if (strcmp(si->inputs[k], sj->output) == 0) {
                    if (j > i) {
                        fprintf(stderr, "embbuild: ordering error: stanza '%s' at line %d consumes output of stanza '%s' at line %d, but appears later in the manifest\n",
                                si->name, si->line, sj->name, sj->line);
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}

/* ---------------------------- Stamps ---------------------------------
 * Hash the inputs, args, and tool version. The hash is the stamp. */
static void stamp_patch(char *out, size_t n, const char *name) {
    snprintf(out, n, "/data/build/stamps/%s/%s.stamp", g_project, name);
}
static void ensure_dirs(void) {
    mkdir("/data/build", 0755);
    mkdir("/data/build/stamps", 0755);
    mkdir("/data/build/out", 0755);                 /* staging root (BUILD.md §9) */
    char path[MAX_TOK];
    snprintf(path, sizeof(path), "/data/build/stamps/%s", g_project);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "/data/build/out/%s", g_project);
    mkdir(path, 0755);                              /* per-project staging, mirroring stamps */
}
static int stamp_read(const char *name, uint32_t *out) {
    char path[MAX_TOK];
    stamp_patch(path, sizeof(path), name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    unsigned v; int ok = (fscanf(f, "%x", &v) == 1);
    fclose(f);
    if (!ok) return -1;
    /* Full word, through a typed pointer. The earlier char* version stored ONE
     * byte into a 4-byte stamp: the other three read as stack garbage, the
     * compare never matched, and every run rebuilt the world -- the up-to-date
     * case (acceptance b) was unreachable. */
    *out = (uint32_t)v;
    return 0;
}
static int stamp_write(const char *name, uint32_t stamp) {
    char path[MAX_TOK];
    stamp_patch(path, sizeof(path), name);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%08x", (unsigned int)stamp);
    fclose(f);
    return 0;
}

/*---------------------------- RUNNING A Stanza ---------------------------------*/
static int run_spawn(struct stanza *s) {
    char *argv[MAX_ITEMS + 1];
    for (int i = 0; i < s->n_args; i++) {
        argv[i] = s->argv[i];
    }
    argv[s->n_args] = NULL;
    /*  argv[0] is an ABSOLUTE path: there is no path: there is no PATH (USERSPACE.md §4.2)
     * and EmbBuild will not invent one. */
    int64_t child = embk_spawn(argv[0], argv, NULL, 0);
    if (child < 0) {
        fprintf(stderr, "embbuild: failed to spawn '%s': %d\n", argv[0], (int)-child);
        return -1;
    }
    int64_t status = embk_wait((int)child);
    if (status != 0) {
        fprintf(stderr, "embbuild: command '%s' exited with status %lld\n", argv[0], (long long)status);
        return -1;
    }
    return 0;
}

/* install = internal copy. Not spawn, for a structural reason: `cp` is a 
 * shell BUILTIN -- there is no /cp.elf to spawn. The one recipe kind that 
 * isn't an argv is the one the OS provides internally. */
static int run_install(struct stanza *s) {
    if (s->n_inputs != 1) {
        fprintf(stderr, "embbuild: install stanza '%s' must have exactly one input\n", s->name);
        return -1;
    }
    /* Boundary enforcement, BUILD.md §3: EmbBuild never writes into /system.
     * Checked against the OUTPUT (an install stanza has no argv -- the earlier
     * argv[1] probe read an empty field), and with a 8-byte "/system/" prefix:
     * the old strncmp(.., "/system", 8) compared 8 bytes INCLUDING the NUL, so
     * it matched only the literal string "/system" and waved every real path
     * ("/system/bin/x" differs at byte 7: '/' vs NUL) straight through. */
    if (strncmp(s->output, "/system/", 8) == 0 || strcmp(s->output, "/system") == 0) {
        fprintf(stderr, "embbuild: install stanza '%s' cannot write into /system -- "
                        "stage to /data/build/out and ADOPT (BUILD.md §3)\n", s->name);
        return -1;
    }
    
    int in = open(s->inputs[0], O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "embbuild: failed to open input '%s'\n", s->inputs[0]);
        return -1;
    }
    int out = open(s->output, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) {
        fprintf(stderr, "embbuild: failed to open output '%s'\n", s->output);
        close(in);
        return -1;
    }
    static char buf[4096];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            n = -1; break;
        }
    }
    close(in);
    close(out);
    if (n < 0) {
        fprintf(stderr, "embbuild: error copying '%s' to '%s'\n", s->inputs[0], s->output);
        return -1;
    }
    return 0;
}

/*--------------------------- main walk ---------------------------------*/
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <manifest>\n", argv[0]);
        return 1;
    }
    crc32c_init();
    if (parse_manifest(argv[1]) != 0) return 1;
    if (ordering_check() != 0) return 1;
    /* AFTER the parse, not before: ensure_dirs() builds per-PROJECT paths from
     * g_project, which is empty until the manifest names it. Called first, it
     * created only the roots -- tcc then failed to write into a staging dir
     * that didn't exist (and the stamps flavor of the same bug hid for a
     * while, because a failed stamp write is deliberately non-fatal). */
    ensure_dirs();

    int n_ran = 0, n_skipped = 0;   /* counts, not flags: "0 ran" (§10) is a number */
    for (int i = 0; i < g_n_stanzas; i++) {
        struct stanza *s = &g_stanzas[i];
        
        /* Hash AT THIS STANZA'S TURN: inputs as they exist right now,
         * including bytes produced earlier in this run. then argv, then
         * the tool identity -- a flag change or tool upgrade must rebuild
         * (the false-fresh case make fails; BUILD.md $4). */
        uint32_t crc = 0xffffffffu;
        for (int j = 0; j < s->n_inputs; j++) {
            crc = crc_feed(crc, s->inputs[j], strlen(s->inputs[j]) + 1);
            if (crc_feed_file(&crc, s->inputs[j]) != 0) {
                fprintf(stderr, "embbuild: failed to hash input '%s' for stanza '%s'\n", s->inputs[j], s->name);
                return 1;   /* missing input = FATAL */
            }
        }
        for (int j = 0; j < s->n_args; j++) {
            crc = crc_feed(crc, s->argv[j], strlen(s->argv[j]) + 1);
        }
        crc = crc_feed(crc, EMBBUILD_VERSION, sizeof EMBBUILD_VERSION);
        crc ^= 0xffffffffu;

        /* Skip iff stamp matches AND the output still exists; a stamp
         * that outlives its artifact must not veto the rebuild. */
        uint32_t old_stamp; struct stat st;
        if (stamp_read(s->name, &old_stamp) == 0 && old_stamp == crc &&
            stat(s->output, &st) == 0) {
            n_skipped++;
            printf("embbuild: skipping '%s' (up to date)\n", s->name);
            continue;
        }

        printf("[embbuild] %-16s %s -> %s\n", s->name, s->kind, s->output);
        int rc = (strcmp(s->kind, "install") == 0) ? run_install(s) : run_spawn(s);
        if (rc != 0) {
            fprintf(stderr, "embbuild: stanza '%s' failed\n", s->name);
            return 1;
        }
        if (stamp_write(s->name, crc) != 0) {
            fprintf(stderr, "embbuild: failed to write stamp for stanza '%s'\n", s->name);
            
        }
        n_ran++;      /* the build succeeded; a failed stamp write is not fatal */
        
    }
    printf("[embbuild] %s: %d ran, %d up_to_date\n", g_project, n_ran, n_skipped);  
    return 0;
}