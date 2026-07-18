/* posixdemo.c -- ring-3 self-check for the POSIX layer in user/lib/syscalls.c.
 *
 * These calls were written to link CPython, but they are now part of EVERY
 * app's libc, and until this program ran they had only ever been COMPILED.
 * Same contract as cxxdemo.elf: print each check, exit 0 only if all pass, so
 * `test posix` is just "spawn it and read the code".
 *
 * What's actually worth testing here isn't "does opendir return non-NULL" --
 * it's the two places the layer could be quietly WRONG:
 *
 *  1. THE TRUNCATION LOOP. The kernel's readdir truncates SILENTLY: given a
 *     buffer for N it returns exactly N with no error and no "there was more"
 *     flag. opendir() therefore starts at 64 and must grow-and-retry until the
 *     count comes back SHORT. So this test builds a directory with MORE than 64
 *     entries -- the only way to prove the retry works instead of silently
 *     losing files. A layer that skipped the loop passes every small-directory
 *     test and then eats your files.
 *  2. d_type TRANSLATION. Native VFS_DT_* (REG=1, DIR=2) and POSIX DT_* (REG=8,
 *     DIR=4) agree on NO value, so an accidental pass-through still "works" for
 *     DT_UNKNOWN and lies for everything else.
 *
 * The refusals are tested too: a call that's supposed to fail with ENOSYS is
 * only honest if it actually does, and a stub that quietly returns 0 would sail
 * through a test that only checks the happy path.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <dirent.h>
#include <fcntl.h>     /* open(), O_RDONLY -- the relative-path checks */
#include <wchar.h>     /* wcscmp/wcrtomb/mbstate_t -- the _Py_wfopen checks */
#include <utime.h>
#include <time.h>
#include <sched.h>
#include <sys/stat.h>

static int failures = 0;

/* Thread-local storage. These two are the whole point of the TLS section below:
 * one INITIALISED (.tdata -- crt0 must copy the image) and one ZERO (.tbss --
 * crt0 must zero it), because those are separate code paths and a bug in either
 * is invisible if you only test the other. A third, over-aligned one checks that
 * crt0 rounds the block with the linker's own __tls_align. */
static __thread int tls_initialised = 0x1234ABCD;   /* .tdata */
static __thread int tls_zeroed;                     /* .tbss  */
static __thread long tls_aligned __attribute__((aligned(32)));

static void ck(const char *what, int ok) {
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

/* A call we EXPECT to fail: it must return -1 AND set the errno we promised.
 * Returning 0 (pretending) or failing with a random errno both count as bugs. */
static void ck_fails(const char *what, int ret, int want_errno, int got_errno) {
    int ok = (ret == -1 && got_errno == want_errno);
    printf("  [%s] %s (rc=%d errno=%d, want -1/%d)\n",
           ok ? "ok" : "FAIL", what, ret, got_errno, want_errno);
    if (!ok) failures++;
}

/* Use this rather than calling ck_fails(what, some_call(), want, errno) directly.
 * C does not specify ARGUMENT EVALUATION ORDER, and gcc evaluates right-to-left
 * here: `errno` would be read BEFORE the call that sets it, reporting the 0 we
 * had just cleared. That cost a full boot cycle and made nine correct functions
 * look broken. The temporaries below force the sequence: clear, call, THEN read. */
#define CK_FAILS(what, call, want_errno) do {   \
        errno = 0;                              \
        int _rc = (call);                       \
        int _err = errno;                       \
        ck_fails((what), _rc, (want_errno), _err); \
    } while (0)

#define TESTDIR "/posixtmp"
#define NENT    70          /* > 64 == opendir()'s initial cap: forces the retry */

/* Until crt0 set up %fs, EVERY line here faulted at CR2=0 -- a TLS read compiles
 * to `mov %fs:0x0,%reg` and an unset FS_BASE makes that linear address 0. This
 * is CPython's exact blocker, in 20 lines instead of 7.5 MB. */
static void test_tls(void) {
    printf("thread-local storage:\n");

    /* Reaching %fs at all. If crt0's setup_tls() or sys_set_fs_base is broken,
     * we never print anything past here -- we fault. */
    ck(".tdata var readable (we got past mov %fs:0x0)", tls_initialised == 0x1234ABCD);
    printf("       (tls_initialised = 0x%X, want 0x1234ABCD)\n", tls_initialised);

    /* .tbss must be ZEROED, not left as heap garbage. crt0 memsets the tail of
     * the block; if it forgot, this is whatever malloc handed back. */
    ck(".tbss var zero-initialised", tls_zeroed == 0);

    /* Writes must stick and stay independent. */
    tls_zeroed = 42;
    tls_aligned = 0x5A5A5A5A5AL;
    ck(".tbss var writable", tls_zeroed == 42);
    ck("over-aligned TLS var writable", tls_aligned == 0x5A5A5A5A5AL);
    ck(".tdata var unaffected by the others", tls_initialised == 0x1234ABCD);

    /* The 32-byte-aligned variable proves crt0 rounded the block with the
     * linker's __tls_align. Get that wrong and the address is merely
     * mis-aligned -- no fault, just a variable silently sharing bytes with its
     * neighbour, which is exactly the kind of bug worth catching here. */
    ck("over-aligned TLS var is actually 32-byte aligned",
       ((unsigned long)&tls_aligned & 31u) == 0);

    /* The TCB self-pointer: *(void**)TP == TP is the ABI contract every TLS
     * access depends on. Read it back through %fs the same way the compiler
     * does, and confirm it points at itself. */
    unsigned long tp = 0;
    __asm__ volatile ("mov %%fs:0x0, %0" : "=r"(tp));
    ck("TCB self-pointer readable via %fs:0", tp != 0);
    ck("TCB self-pointer points at itself", tp != 0 && *(unsigned long *)tp == tp);

    /* Variant II: the block lives BELOW the thread pointer, so every TLS
     * variable must sit at a LOWER address than TP. */
    ck("TLS vars live below the thread pointer (variant II)",
       tp != 0 && (unsigned long)&tls_initialised < tp);
    printf("       (tp = %p, &tls_initialised = %p)\n", (void *)tp, (void *)&tls_initialised);
}

static void test_clocks(void) {
    printf("clocks:\n");
    struct timespec a, b, res;

    ck("clock_gettime(CLOCK_MONOTONIC)", clock_gettime(CLOCK_MONOTONIC, &a) == 0);

    /* Monotonic must ADVANCE across a sleep -- a clock stuck at a constant is
     * the classic "implemented" clock that breaks every timeout in userland. */
    usleep(30000);   /* 30ms; our sleep is ms-granular */
    ck("clock_gettime(CLOCK_MONOTONIC) again", clock_gettime(CLOCK_MONOTONIC, &b) == 0);
    long long da = (long long)a.tv_sec * 1000000000LL + a.tv_nsec;
    long long db = (long long)b.tv_sec * 1000000000LL + b.tv_nsec;
    ck("monotonic advanced across usleep(30ms)", db > da);
    printf("       (advanced %lld ms)\n", (db - da) / 1000000);

    ck("tv_nsec in range", b.tv_nsec >= 0 && b.tv_nsec < 1000000000L);
    ck("clock_gettime(CLOCK_REALTIME)", clock_gettime(CLOCK_REALTIME, &a) == 0);

    /* Resolution must report what we DELIVER (1ms), not a vanity 1ns: callers
     * pace work off this. */
    ck("clock_getres(MONOTONIC)", clock_getres(CLOCK_MONOTONIC, &res) == 0);
    ck("  reports 1ms (not a fictional 1ns)", res.tv_sec == 0 && res.tv_nsec == 1000000L);
    ck("clock_getres(REALTIME)", clock_getres(CLOCK_REALTIME, &res) == 0);
    ck("  reports 1s (the RTC's real granularity)", res.tv_sec == 0 && res.tv_nsec == 1000000000L);

    /* An unknown clock id must be REFUSED, not quietly answered with the
     * monotonic value. Note we can't name CLOCK_PROCESS_CPUTIME_ID here: newlib
     * gates it behind _POSIX_CPUTIME, which EmbLink deliberately does not claim
     * (there is no per-process CPU accounting, so any number would be fiction) --
     * so no app on this OS can even ask for it. What's testable, and what
     * actually protects callers, is that the default branch says EINVAL. */
    CK_FAILS("clock_gettime(unknown id) refused", clock_gettime((clockid_t)9999, &a), EINVAL);
}

/* THE WORKING DIRECTORY. A libc fact, not a kernel one: the kernel stays the
 * one absolute-only path parser and never sees a relative path. Per-process
 * comes free -- every process has its own copy of this libc's g_cwd.
 *
 * The checks that matter are the ones proving relative paths RESOLVE against
 * it, and that ".." pops. git finds a repo root by walking UP; ".." is its
 * normal idiom, and absolute paths used to reach the kernel unnormalized. */
static void test_cwd(void) {
    printf("working directory:\n");
    char buf[128];

    char *p = getcwd(buf, sizeof buf);
    ck("getcwd() succeeds", p == buf);
    ck("getcwd() starts at \"/\" (nothing inherited unless PWD names it)",
       p && strcmp(p, "/") == 0);
    ck("chdir(\"/\") succeeds", chdir("/") == 0);

    /* Somewhere real to stand. */
    (void)mkdir("/cwdtest", 0755);
    (void)mkdir("/cwdtest/sub", 0755);

    ck("chdir to a real directory == 0", chdir("/cwdtest") == 0);
    ck("getcwd() reports it", getcwd(buf, sizeof buf) && strcmp(buf, "/cwdtest") == 0);

    /* THE POINT: a relative name must resolve against the cwd, not the root. */
    int fd = open("rel.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    ck("open(relative) succeeds", fd >= 0);
    if (fd >= 0) { write(fd, "R", 1); close(fd); }
    struct stat st;
    ck("...and it landed in the CWD, not at /",
       stat("/cwdtest/rel.txt", &st) == 0 && stat("/rel.txt", &st) != 0);

    /* Relative chdir, then ".." back up. */
    ck("chdir(\"sub\") relative == 0", chdir("sub") == 0);
    ck("getcwd() == /cwdtest/sub", getcwd(buf, sizeof buf) && strcmp(buf, "/cwdtest/sub") == 0);
    ck("chdir(\"..\") == 0", chdir("..") == 0);
    ck("\"..\" popped exactly one component", getcwd(buf, sizeof buf) && strcmp(buf, "/cwdtest") == 0);

    /* Normalization of an ABSOLUTE path -- these used to reach the kernel raw. */
    ck("absolute path with .. resolves", stat("/cwdtest/sub/../rel.txt", &st) == 0);
    ck("absolute path with . resolves",  stat("/cwdtest/./rel.txt", &st) == 0);
    /* ".." at the root stays at the root (POSIX: "/.." is "/"), rather than
     * walking off the front of the buffer. */
    ck("chdir(\"/..\") lands at the root, not off the end", chdir("/..") == 0);
    ck("getcwd() == \"/\" after /..", getcwd(buf, sizeof buf) && strcmp(buf, "/") == 0);

    /* chdir VERIFIES: an unchecked store would turn one bad chdir into a pile
     * of confusing ENOENTs far from the cause. */
    CK_FAILS("chdir(missing) -> ENOENT", chdir("/no_such_dir"), ENOENT);
    fd = open("/cwdtest/rel.txt", O_RDONLY);
    if (fd >= 0) close(fd);
    CK_FAILS("chdir(a FILE) -> ENOTDIR", chdir("/cwdtest/rel.txt"), ENOTDIR);
    ck("a refused chdir did NOT move us", getcwd(buf, sizeof buf) && strcmp(buf, "/") == 0);

    CK_FAILS("getcwd(buf,1) -> ERANGE", getcwd(buf, 1) == NULL ? -1 : 0, ERANGE);

    (void)unlink("/cwdtest/rel.txt");
    (void)rmdir("/cwdtest/sub");
    (void)rmdir("/cwdtest");
}

static void test_dir_basics(void) {
    printf("directory basics:\n");
    DIR *d = opendir("/");
    ck("opendir(\"/\")", d != NULL);
    if (!d) return;

    int n = 0, saw_reg = 0, saw_dir = 0, saw_unknown = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        n++;
        if (e->d_type == DT_REG) saw_reg++;
        else if (e->d_type == DT_DIR) saw_dir++;
        else if (e->d_type == DT_UNKNOWN) saw_unknown++;
        if (e->d_name[0] == '\0') { ck("entry has a name", 0); break; }
    }
    printf("       (%d entries: %d reg, %d dir, %d unknown)\n", n, saw_reg, saw_dir, saw_unknown);
    ck("root is not empty", n > 0);

    /* The d_type table is load-bearing: native REG=1 vs DT_REG=8. If it were
     * passed through untranslated, a regular file would read as DT_FIFO(1). */
    ck("d_type translated (saw DT_REG for the .elf files)", saw_reg > 0);

    /* EOF contract: NULL WITHOUT touching errno, so callers can tell end-of-dir
     * from a real error. */
    errno = 0;
    e = readdir(d);
    ck("readdir at EOF -> NULL", e == NULL);
    ck("readdir at EOF leaves errno untouched", errno == 0);

    rewinddir(d);
    int n2 = 0;
    while (readdir(d) != NULL) n2++;
    ck("rewinddir replays the same count", n2 == n);

    ck("closedir", closedir(d) == 0);
}

static void test_dir_errors(void) {
    printf("directory errors:\n");
    DIR *d;
    CK_FAILS("opendir(missing) -> ENOENT",
             (d = opendir("/nonexistent-xyz")) ? 0 : -1, ENOENT);
    if (d) closedir(d);

    /* A FILE is not a directory: this must be ENOTDIR, not ENOENT. */
    CK_FAILS("opendir(a file) -> ENOTDIR",
             (d = opendir("/hello.txt")) ? 0 : -1, ENOTDIR);
    if (d) closedir(d);
}

/* THE important one. */
static void test_truncation_retry(void) {
    printf("readdir truncation retry (>64 entries):\n");

    rmdir(TESTDIR);   /* best-effort cleanup from a previous run */
    if (mkdir(TESTDIR, 0755) != 0 && errno != EEXIST) {
        ck("mkdir " TESTDIR, 0);
        return;
    }
    ck("mkdir " TESTDIR, 1);

    int made = 0;
    for (int i = 0; i < NENT; i++) {
        char p[80];
        snprintf(p, sizeof p, TESTDIR "/d%02d", i);
        if (mkdir(p, 0755) == 0 || errno == EEXIST) made++;
    }
    printf("       (created %d/%d subdirectories)\n", made, NENT);
    ck("created > 64 entries (to overflow opendir's first buffer)", made > 64);

    DIR *d = opendir(TESTDIR);
    ck("opendir(" TESTDIR ")", d != NULL);
    if (d) {
        int n = 0, dirs = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            n++;
            if (e->d_type == DT_DIR) dirs++;
        }
        printf("       (readdir returned %d entries, %d of them DT_DIR)\n", n, dirs);
        /* If the grow-and-retry loop were missing, this would silently be
         * exactly 64 -- the failure the whole test exists to catch. */
        ck("saw ALL entries, not a truncated 64", n >= made);
        ck("subdirectories typed DT_DIR", dirs >= made);
        closedir(d);
    }

    for (int i = 0; i < NENT; i++) {
        char p[80];
        snprintf(p, sizeof p, TESTDIR "/d%02d", i);
        rmdir(p);
    }
    ck("rmdir " TESTDIR, rmdir(TESTDIR) == 0);

    errno = 0;
    DIR *gone = opendir(TESTDIR);
    ck("directory really gone after rmdir", gone == NULL && errno == ENOENT);
    if (gone) closedir(gone);
}

/* The kernel VFS is absolute-only (vfs.c: relative -> EINVAL) while getcwd()
 * reports "/", so the libc must resolve relative names against the root. Before
 * it did, getcwd() said "/" but open("foo") failed EINVAL -- the contradiction
 * that killed CPython startup: frozen getpath.py probes a relative "pyvenv.cfg"
 * inside `except (FileNotFoundError, PermissionError)`, and EINVAL is neither. */
static void test_relative_paths(void) {
    printf("relative paths (resolve against the fixed root):\n");

    struct stat a, b;
    int ra = stat("/hello.txt", &a);
    int rb = stat("hello.txt", &b);          /* the SAME file, named relatively */
    ck("stat(\"hello.txt\") == stat(\"/hello.txt\")",
       ra == 0 && rb == 0 && a.st_size == b.st_size);

    /* The exact shape getpath.py needs: a MISSING relative file must be ENOENT
     * (catchable) and never EINVAL (fatal). */
    CK_FAILS("missing relative file -> ENOENT, not EINVAL",
             stat("pyvenv.cfg", &b), ENOENT);
    CK_FAILS("missing relative open -> ENOENT", open("pyvenv.cfg", O_RDONLY), ENOENT);

    /* "." and ".." are the VFS path layer's job; "./x" becomes "/./x". */
    ck("stat(\"./hello.txt\") resolves", stat("./hello.txt", &b) == 0);

    int fd = open("hello.txt", O_RDONLY);
    ck("open(\"init.elf\") relative succeeds", fd >= 0);
    if (fd >= 0) close(fd);

    DIR *d = opendir(".");
    ck("opendir(\".\") == the root", d != NULL);
    if (d) closedir(d);

    /* An EMPTY path is ENOENT, NOT the root: a naive "prepend a slash" would
     * turn open("") into open("/") and hand back success where POSIX demands
     * failure. */
    CK_FAILS("empty path -> ENOENT (not silently the root)", stat("", &b), ENOENT);
    CK_FAILS("empty path open -> ENOENT", open("", O_RDONLY), ENOENT);

    /* Over-long paths are refused here rather than sent to a kernel that would
     * reject them anyway. */
    char toolong[400];
    memset(toolong, 'a', sizeof toolong - 1);
    toolong[sizeof toolong - 1] = '\0';
    CK_FAILS("over-long path -> ENAMETOOLONG", stat(toolong, &b), ENAMETOOLONG);
}

/* Reading a FILE's contents -- which nothing here covered until CPython caught
 * it. Both layers, separately, because they fail differently:
 *   - raw read(2)  -> our syscall + the kernel's fd layer
 *   - stdio fread  -> newlib's buffering on top (fopen sizes its buffer from
 *                     st_blksize, so a bad stat breaks reads without touching
 *                     read() itself)
 * CPython's getpath.py reads /python.elf._pth with fopen("rb")+fread and got an
 * EMPTY result, which silently produced an empty sys.path and
 * "No module named 'encodings'" -- no error, just nothing. */
static void test_file_read(void) {
    printf("file reads:\n");

    /* A known 16-byte file we ship: "python314.zip\n.\n" */
    static const char *WANT = "python314.zip\n.\n";
    const size_t want_len = 16;

    struct stat st;
    if (stat("/python.elf._pth", &st) != 0) {
        printf("       (/python.elf._pth absent -- CPython not built; skipping)\n");
        return;
    }
    ck("stat reports the right size (16)", (size_t)st.st_size == want_len);
    ck("st_blksize is sane (stdio sizes its buffer from this)", st.st_blksize > 0);

    /* --- raw read(2) --- */
    char buf[64];
    memset(buf, 0, sizeof buf);
    int fd = open("/python.elf._pth", O_RDONLY);
    ck("open() the file", fd >= 0);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof buf - 1);
        printf("       (read() returned %d bytes)\n", (int)n);
        ck("read() returns the whole file", n == (ssize_t)want_len);
        ck("read() content matches", n > 0 && memcmp(buf, WANT, (size_t)n) == 0);
        /* EOF must be 0, not an error -- a caller loops until 0. */
        ssize_t z = read(fd, buf, sizeof buf - 1);
        ck("read() at EOF returns 0", z == 0);
        close(fd);
    }

    /* --- stdio fread, the path CPython actually uses --- */
    memset(buf, 0, sizeof buf);
    FILE *f = fopen("/python.elf._pth", "rb");
    ck("fopen() the file", f != NULL);
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        printf("       (fread() returned %d bytes)\n", (int)n);
        ck("fread() returns the whole file", n == want_len);
        ck("fread() content matches", n > 0 && memcmp(buf, WANT, n) == 0);
        ck("feof() set after reading past the end", feof(f) != 0);
        fclose(f);
    }

    /* fgets is what a line reader uses. */
    f = fopen("/python.elf._pth", "rb");
    if (f) {
        char line[64];
        char *got = fgets(line, sizeof line, f);
        ck("fgets() returns the first line", got != NULL);
        ck("fgets() line is \"python314.zip\\n\"",
           got != NULL && strcmp(line, "python314.zip\n") == 0);
        fclose(f);
    }
}

/* LARGE files: reads past one block, and seeks to large offsets. Everything
 * above only ever touched a 16-byte file, so this whole path was untested --
 * and it is exactly what CPython does at startup: zipimport opens the 10.3 MB
 * python314.zip, seeks to the END for the central directory, then seeks back to
 * each member. Startup currently freezes right where that would happen (see
 * memory/cpython-port.md), so prove the primitives here first: a bug is far
 * cheaper to find in 40 lines than inside an interpreter. */
static void test_large_file(void) {
    printf("large file I/O (what zipimport needs):\n");

    struct stat st;
    if (stat("/python314.zip", &st) != 0) {
        printf("       (/python314.zip absent -- CPython not built; skipping)\n");
        return;
    }
    off_t size = st.st_size;
    printf("       (size = %ld bytes, %.1f MB)\n", (long)size, (double)size / 1048576.0);
    ck("stat reports a multi-MB size", size > 1000000);

    int fd = open("/python314.zip", O_RDONLY);
    ck("open the zip", fd >= 0);
    if (fd < 0) return;

    /* SEEK_END: zipimport's very first move, to find the End Of Central Dir. */
    off_t end = lseek(fd, 0, SEEK_END);
    printf("       (lseek(SEEK_END) -> %ld)\n", (long)end);
    ck("lseek(0, SEEK_END) == file size", end == size);

    /* Seek BACK to a large offset and read the EOCD signature there. A seek
     * that silently clamps or wraps shows up right here. */
    off_t eocd = size - 22;
    off_t got = lseek(fd, eocd, SEEK_SET);
    ck("lseek to a large absolute offset", got == eocd);
    unsigned char sig[4] = {0};
    ssize_t n = read(fd, sig, 4);
    ck("read at a large offset returns data", n == 4);
    /* PK\x05\x06 == End Of Central Directory. If the seek landed wrong, this is
     * garbage -- and zipimport would conclude "not a zip" and silently skip it,
     * which is precisely the kind of quiet failure we keep hitting. */
    ck("EOCD signature is PK\\x05\\x06 at size-22",
       sig[0] == 'P' && sig[1] == 'K' && sig[2] == 5 && sig[3] == 6);

    /* Read the WHOLE file. This crosses thousands of blocks; nothing has ever
     * done that. Also the honest way to see whether large reads are merely slow
     * or actually stuck. */
    /* Read forward, REPORTING AS WE GO. Printing only at the end taught us
     * nothing when this had to be killed -- and the per-chunk timing is the
     * actual measurement we want: EMBKFS's read_impl re-enters the object at an
     * ABSOLUTE offset every call (embkfs_read_object_at), so if that walks from
     * the start, cost grows with offset and sequential reads are O(n^2).
     * A RISING ms-per-MB is that smoking gun; a flat one exonerates it. */
    ck("rewind to 0", lseek(fd, 0, SEEK_SET) == 0);
    static char buf[8192];
    off_t total = 0;
    int reads = 0;
    off_t next_report = 1048576;           /* every 1 MB */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long prev_ms = 0;
    int rising = 0, samples = 0;

    /* Cap the work: 3 MB is enough to see the trend without a 10-minute test. */
    const off_t LIMIT = 3 * 1048576;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) { ck("read() error mid-file", 0); break; }
        if (r == 0) break;                 /* EOF */
        total += r;
        reads++;
        if (total >= next_report) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                             (t1.tv_nsec - t0.tv_nsec) / 1000000);
            long delta = ms - prev_ms;     /* ms for THIS megabyte */
            printf("       (%ld MB read, +%ld ms for this MB)\n",
                   (long)(total / 1048576), delta);
            if (samples > 0 && delta > prev_ms / 2) rising++;
            prev_ms = ms;
            samples++;
            next_report += 1048576;
        }
        if (total >= LIMIT) break;
    }
    printf("       (read %ld bytes in %d reads)\n", (long)total, reads);
    ck("large sequential read makes progress", total >= LIMIT);
    /* The verdict this test exists for. Per-MB cost should be CONSTANT; if each
     * successive megabyte costs meaningfully more, reads are O(offset). */
    /* DO NOT read "flat" as "fine", and judge the ABSOLUTE ms/MB. An extent's
     * checksum covers the WHOLE extent, so a read must verify every byte of any
     * extent it touches. mkfs used to give a file ONE extent, which made that
     * cost O(filesize) -- a CONSTANT per read, so per-MB timing looked flat while
     * still being O(n^2) overall. mkfs now caps extents (EXTENT_MAX_BYTES), so
     * the cost is bounded, but flat STILL doesn't imply O(len): judge the number.
     * History, as a scale: ~36000 ms/MB (2026-07-16) meant every 8 KB read was
     * re-reading the whole 10 MB extent. The kernel-side `test ioperf` reports
     * read amplification directly and is the better instrument for this. */
    printf("       (per-MB cost %s -- flat does NOT imply O(len); judge the\n"
           "        ABSOLUTE ms/MB, and see `test ioperf` for amplification)\n",
           rising >= 2 ? "RISING" : "flat");

    close(fd);
}

/* Wide-char <-> multibyte conversion. Obscure-looking, but it is exactly what
 * stands between CPython and its stdlib: getpath.c's readlines() opens the
 * ._pth with _Py_wfopen(path, L"rb"), which does NOT call fopen directly -- it
 * first wcstombs()es the MODE and locale-encodes the PATH (wcrtomb per char),
 * and returns NULL if either conversion fails. On EmbLink that read fails
 * (DIAG showed pth=None) even though plain fopen()+fread of the same file works
 * perfectly (see test_file_read above), so the conversion layer is the suspect.
 *
 * newlib's default locale is "C", where every one of these is a plain
 * byte-for-byte ASCII mapping -- there is no excuse for a failure. */
static void test_widechar(void) {
    printf("wide-char conversion (what _Py_wfopen needs):\n");

    /* The MODE conversion, verbatim: _Py_wfopen does wcstombs(cmode, L"rb", 10)
     * and treats (size_t)-1 as fatal. */
    char mb[16];
    memset(mb, 0, sizeof mb);
    size_t r = wcstombs(mb, L"rb", sizeof mb);
    printf("       (wcstombs(L\"rb\") -> %ld, got \"%s\")\n", (long)r, mb);
    ck("wcstombs(L\"rb\") converts", r == 2);
    ck("wcstombs(L\"rb\") == \"rb\"", strcmp(mb, "rb") == 0);

    /* The PATH conversion: same call shape, on the real path. */
    memset(mb, 0, sizeof mb);
    size_t r2 = wcstombs(mb, L"/init.elf", sizeof mb);
    ck("wcstombs(a path) converts", r2 == 9);
    ck("wcstombs(a path) round-trips", strcmp(mb, "/init.elf") == 0);

    /* mbstowcs is the other direction (used to build the wide path). */
    wchar_t wb[32];
    size_t r3 = mbstowcs(wb, "/python.elf._pth", 32);
    ck("mbstowcs converts", r3 == 16);
    ck("mbstowcs round-trips", r3 == 16 && wcscmp(wb, L"/python.elf._pth") == 0);

    /* wcrtomb is what CPython's locale encoder actually loops over per char. */
    mbstate_t st;
    memset(&st, 0, sizeof st);
    char one[8];
    size_t r4 = wcrtomb(one, L'/', &st);
    ck("wcrtomb('/') converts one char", r4 == 1 && one[0] == '/');

    /* MB_CUR_MAX == 1 in the C locale; a 0 here would make encoders allocate
     * nothing and silently produce empty strings. */
    printf("       (MB_CUR_MAX = %d)\n", (int)MB_CUR_MAX);
    ck("MB_CUR_MAX is sane (>= 1)", MB_CUR_MAX >= 1);
}

static void test_stat_access(void) {
    printf("stat / access:\n");
    struct stat a, b;
    ck("stat(/hello.txt)", stat("/hello.txt", &a) == 0);
    ck("lstat(/hello.txt)", lstat("/hello.txt", &b) == 0);
    /* lstat == stat is exact here: nothing follows symlinks. */
    ck("lstat agrees with stat (no link following exists)",
       a.st_size == b.st_size && a.st_mode == b.st_mode);
    ck("access(/hello.txt, F_OK)", access("/hello.txt", F_OK) == 0);
    CK_FAILS("access(missing) -> ENOENT", access("/nope-xyz", F_OK), ENOENT);
}

static void test_sysparams(void) {
    printf("system parameters:\n");
    ck("getpagesize() == 4096 (the real VMM page size)", getpagesize() == 4096);
    ck("sysconf(_SC_PAGESIZE) == 4096", sysconf(_SC_PAGESIZE) == 4096);
    ck("sysconf(_SC_OPEN_MAX) == 64 (FD_MAX_OPEN)", sysconf(_SC_OPEN_MAX) == 64);

    /* We cannot ask the kernel its CPU count, so this must REFUSE rather than
     * guess 1 -- a guess is indistinguishable from a measurement. */
    errno = 0;
    long r = sysconf(_SC_NPROCESSORS_ONLN);
    ck_fails("sysconf(_SC_NPROCESSORS_ONLN) refuses to guess", (int)r, EINVAL, errno);

    ck("umask() returns 0 and cannot fail", umask(022) == 0);
    ck("sched_yield()", sched_yield() == 0);
}

/* fcntl's descriptor flags. This is the call that silently broke CPython: it
 * opens every file via _Py_wfopen, which calls make_non_inheritable ->
 * fcntl(fd, F_GETFD) right after fopen() succeeds, and closed the file again
 * when our ENOSYS came back. FD_CLOEXEC is answerable here because EmbLink has
 * no exec -- see the long comment on fcntl() in syscalls.c. */
static void test_fcntl(void) {
    printf("fcntl descriptor flags:\n");

    int fd = open("/hello.txt", O_RDONLY);
    ck("open a file to test against", fd >= 0);
    if (fd < 0) return;

    int flags = fcntl(fd, F_GETFD);
    printf("       (F_GETFD -> 0x%X)\n", flags);
    ck("F_GETFD succeeds", flags != -1);
    /* Every fd is effectively close-on-exec: there is no exec to survive. */
    ck("F_GETFD reports FD_CLOEXEC", flags != -1 && (flags & FD_CLOEXEC));

    ck("F_SETFD(FD_CLOEXEC) accepted", fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    ck("F_SETFD(0) accepted too (no exec => unobservable either way)",
       fcntl(fd, F_SETFD, 0) == 0);

    /* Still refused: a false success here would make a caller believe it had
     * non-blocking I/O and then hang in a blocking read. */
    CK_FAILS("F_GETFL still refused (O_NONBLOCK is unimplementable)",
             fcntl(fd, F_GETFL), ENOSYS);

    close(fd);

    /* A closed/bogus fd must be EBADF, not a cheerful answer. */
    CK_FAILS("F_GETFD on a closed fd -> EBADF", fcntl(fd, F_GETFD), EBADF);
    CK_FAILS("F_GETFD on a bogus fd -> EBADF", fcntl(9999, F_GETFD), EBADF);
}

/* Every one of these is supposed to fail. A stub that returned 0 would pass a
 * happy-path test and then mislead its caller at the worst moment. */
static void test_honest_refusals(void) {
    printf("honest refusals (must NOT pretend):\n");
    struct utimbuf ub = { 0, 0 };
    CK_FAILS("utime -> ENOSYS", utime("/init.elf", &ub), ENOSYS);
    CK_FAILS("fsync -> ENOSYS (never fake durability)", fsync(1), ENOSYS);
    CK_FAILS("fdatasync -> ENOSYS", fdatasync(1), ENOSYS);
    CK_FAILS("dup -> ENOSYS", dup(1), ENOSYS);
    CK_FAILS("symlink -> ENOSYS", symlink("/init.elf", "/l"), ENOSYS);
    CK_FAILS("chroot -> ENOSYS", chroot("/"), ENOSYS);
    CK_FAILS("pause -> ENOSYS (no signals; would be a hang)", pause(), ENOSYS);

    DIR *d = opendir("/");
    if (d) {
        CK_FAILS("dirfd -> ENOTSUP (a snapshot has no fd)", dirfd(d), ENOTSUP);
        closedir(d);
    }
}

/* The ENVIRONMENT. Two legitimate states, and this checks BOTH from one binary
 * depending on how it was spawned:
 *
 *   `test posix`  -- spawned with NO environment (the EmbLink default: a child
 *                    gets only what its parent names). getenv() must answer NULL
 *                    honestly, and `environ` must still be a walkable empty
 *                    vector rather than NULL.
 *   `test env`    -- spawned with an explicit environment, which must arrive
 *                    intact, in order, with no extras.
 *
 * Both are correct behaviour; which one runs is decided by the parent, so the
 * probe below detects it rather than assuming. */
static void test_env(void) {
    printf("environment (explicit at spawn, never inherited):\n");

    ck("environ is non-NULL (walkable even when empty)", environ != NULL);
    if (!environ) return;

    int n = 0;
    while (environ[n]) n++;

    const char *marker = getenv("EMBK_ENV_TEST");
    if (!marker) {
        /* Spawned WITHOUT an environment. */
        printf("       (spawned with NO environment -- the default)\n");
        ck("environ is an empty vector (environ[0] == NULL)", n == 0);
        ck("getenv(\"HOME\") -> NULL (unset, not fabricated)", getenv("HOME") == NULL);
        ck("getenv(\"PATH\") -> NULL", getenv("PATH") == NULL);
        ck("getenv(\"\") -> NULL", getenv("") == NULL);
        return;
    }

    /* Spawned WITH an environment -- `test env` passes exactly these. */
    printf("       (spawned WITH an environment: %d entr%s)\n", n, n == 1 ? "y" : "ies");
    ck("getenv(\"EMBK_ENV_TEST\") == \"1\"", strcmp(marker, "1") == 0);
    const char *home = getenv("HOME");
    ck("getenv(\"HOME\") == \"/\"", home && strcmp(home, "/") == 0);
    const char *path = getenv("PATH");
    ck("getenv(\"PATH\") == \"/\"", path && strcmp(path, "/") == 0);
    const char *empty = getenv("EMBK_EMPTY");
    /* An empty VALUE is not an unset variable -- getenv must return "" not NULL. */
    ck("getenv(\"EMBK_EMPTY\") == \"\" (set-but-empty != unset)",
       empty && empty[0] == '\0');
    ck("getenv(\"EMBK_ABSENT\") -> NULL (still honest about unset)",
       getenv("EMBK_ABSENT") == NULL);
    /* A prefix of a real name must not match it. */
    ck("getenv(\"HOM\") -> NULL (prefix is not a match)", getenv("HOM") == NULL);
    ck("environ carried exactly the 4 entries passed", n == 4);
}

/* MUTATING the environment. The kernel hands `environ` and its strings to us in
 * the child's STACK page, which newlib knows nothing about -- so the question is
 * whether setenv/unsetenv/putenv trample memory they did not allocate.
 *
 * Reading newlib says no: setenv mallocs a FRESH array and copies the pointers
 * rather than growing ours (it never frees the original), unsetenv only shifts
 * pointers within the array, and putenv strdup()s instead of adopting the
 * caller's string. Its one in-place write is bounded by `strlen(old) >= strlen(new)`.
 * That is a reading, not evidence -- hence this test. Runs in BOTH spawn modes:
 * with no environment, `environ` is the static empty vector instead, which is
 * the same question with a different array. */
static void test_env_mutation(void) {
    printf("environment mutation (over memory newlib did not allocate):\n");

    /* Snapshot a kernel-delivered string BEFORE touching anything, so we can
     * prove mutation didn't corrupt its neighbours in that stack page. */
    const char *before = getenv("HOME");
    char home_copy[32] = {0};
    if (before) strncpy(home_copy, before, sizeof home_copy - 1);

    /* NEW variable: the path that must NOT grow/free the kernel's array. */
    ck("setenv(new) == 0", setenv("EMBK_NEW", "alpha", 1) == 0);
    const char *v = getenv("EMBK_NEW");
    ck("getenv(new) == \"alpha\"", v && strcmp(v, "alpha") == 0);

    /* Overwrite SHORTER: newlib writes in place, into whatever page holds it. */
    ck("setenv(overwrite shorter) == 0", setenv("EMBK_NEW", "b", 1) == 0);
    v = getenv("EMBK_NEW");
    ck("getenv == \"b\" (in-place overwrite)", v && strcmp(v, "b") == 0);

    /* Overwrite LONGER: takes the malloc-a-new-string path instead. */
    ck("setenv(overwrite longer) == 0", setenv("EMBK_NEW", "aaaaaaaaaaaaaaaa", 1) == 0);
    v = getenv("EMBK_NEW");
    ck("getenv == 16 a's (reallocated value)", v && strcmp(v, "aaaaaaaaaaaaaaaa") == 0);

    /* rewrite==0 must NOT clobber an existing value. */
    ck("setenv(existing, rewrite=0) == 0", setenv("EMBK_NEW", "ignored", 0) == 0);
    v = getenv("EMBK_NEW");
    ck("value unchanged when rewrite==0", v && strcmp(v, "aaaaaaaaaaaaaaaa") == 0);

    /* A '=' in the NAME is malformed -> EINVAL, not a corrupted entry. */
    CK_FAILS("setenv(name with '=') -> EINVAL", setenv("A=B", "x", 1), EINVAL);

    ck("putenv(\"EMBK_PUT=pv\") == 0", putenv((char *)"EMBK_PUT=pv") == 0);
    v = getenv("EMBK_PUT");
    ck("getenv(putenv'd) == \"pv\"", v && strcmp(v, "pv") == 0);

    ck("unsetenv(new) == 0", unsetenv("EMBK_NEW") == 0);
    ck("getenv after unsetenv -> NULL", getenv("EMBK_NEW") == NULL);
    ck("unsetenv of an absent name still succeeds", unsetenv("EMBK_NEVER_SET") == 0);

    /* THE INTEGRITY CHECK: a kernel-delivered entry the mutations never named
     * must still read back byte-identical. If setenv had grown/freed/moved the
     * stack array, or written past a string, this is where it shows. */
    if (before) {
        const char *after = getenv("HOME");
        ck("kernel-delivered HOME survives mutation intact",
           after && strcmp(after, home_copy) == 0);
    } else {
        ck("HOME still unset after mutation (nothing fabricated)",
           getenv("HOME") == NULL);
    }
}

/* SIGNALS. EmbLink has no signal DELIVERY -- nothing can interrupt a running
 * process from outside. What newlib gives us on top of that is real, though, and
 * this pins exactly how much:
 *
 *   signal()  records a handler          -- works, entirely in userspace
 *   raise()   dispatches SYNCHRONOUSLY   -- works: same thread, no async needed
 *   SIG_DFL   -> kill(self) -> _exit(128+signo)
 *
 * So a self-signal is REAL signal semantics; only asynchronous delivery is
 * missing. Registering a SIGINT handler therefore succeeds and the handler never
 * runs -- honest only because nothing can send SIGINT. The moment Ctrl-C exists,
 * that promise comes due. */
static volatile sig_atomic_t g_sig_seen;
static void sig_handler(int s) { g_sig_seen = s; }

/* The kernel numbers errors LINUX-style; newlib numbers its own differently
 * above ~34. syscalls.c translates by NAME. Without it, a kernel ENAMETOOLONG
 * (36) surfaced as EIDRM and a kernel ENOSYS (38) as EL2NSYNC -- wrong, and
 * invisible until someone read errno on a failure path. This pins the
 * translation using a KERNEL-SOURCED error (the hand-written stubs in
 * syscalls.c set newlib constants directly and were never affected, which is
 * precisely why the bug hid). */
static void test_errno_translation(void) {
    printf("kernel->newlib errno translation:\n");

    /* The error MUST originate in the KERNEL. An over-long path looks like the
     * obvious probe and is useless here: path_abs() in syscalls.c rejects it
     * locally with newlib's own ENAMETOOLONG and never reaches the kernel, so it
     * passes whether or not the translation exists.
     *
     * rmdir() of a NON-EMPTY directory is genuine: embkfs.c:3632 returns the
     * kernel's ENOTEMPTY(39) and nothing in userspace pre-empts it. newlib's
     * ENOTEMPTY is 90; untranslated, 39 reads as EL3HLT. */
    /* Build the condition here rather than relying on a directory an earlier
     * test left lying around -- the first draft of this did exactly that and got
     * ENOENT, because the dir had already been cleaned up. */
    (void)mkdir("/errnodir", 0755);
    int f = open("/errnodir/child", O_CREAT | O_WRONLY, 0644);
    ck("set up a NON-EMPTY dir for the probe", f >= 0);
    if (f >= 0) close(f);

    errno = 0;
    int rc = rmdir("/errnodir");
    int e = errno;
    ck("rmdir of a non-empty dir fails", rc != 0);
    ck("errno is ENOTEMPTY, not the raw kernel number (39 -> EL3HLT)",
       rc != 0 && e == ENOTEMPTY);
    if (rc != 0 && e != ENOTEMPTY)
        printf("       (got errno=%d; kernel sends 39, newlib wants %d)\n",
               e, ENOTEMPTY);

    (void)unlink("/errnodir/child");
    (void)rmdir("/errnodir");
}

/* rename(), including the REPLACE that git's lockfile protocol is built on.
 *
 * The kernel does the replace in ONE commit (embkfs_rename): the name repoint
 * and the victim's teardown are the same transaction, so a crash cannot leave
 * both names gone. The libc used to fake it with unlink-then-rename, which is
 * exactly the guarantee the caller was relying on, borrowed against. */
static void test_rename(void) {
    printf("rename (atomic replace -- the lockfile primitive):\n");

    /* plain move */
    int fd = open("/rn_a.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    ck("create source", fd >= 0);
    if (fd >= 0) { write(fd, "AAAA", 4); close(fd); }

    ck("rename to a FREE name == 0", rename("/rn_a.txt", "/rn_b.txt") == 0);
    ck("source is gone", access("/rn_a.txt", F_OK) != 0);
    ck("destination is there", access("/rn_b.txt", F_OK) == 0);

    /* THE REPLACE: destination exists AND has different content. */
    fd = open("/rn_c.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    ck("create a victim with content", fd >= 0);
    if (fd >= 0) { write(fd, "VICTIM-CONTENT", 14); close(fd); }

    ck("rename ONTO an existing file == 0 (replace, not EEXIST)",
       rename("/rn_b.txt", "/rn_c.txt") == 0);
    ck("source gone after replace", access("/rn_b.txt", F_OK) != 0);

    /* The destination must now be the SOURCE's bytes -- if the victim's data
     * survived, the name was repointed at the wrong object. */
    char buf[32] = {0};
    fd = open("/rn_c.txt", O_RDONLY);
    ck("replaced destination opens", fd >= 0);
    if (fd >= 0) {
        int n = read(fd, buf, sizeof buf - 1);
        close(fd);
        ck("destination holds the SOURCE's bytes", n == 4 && strcmp(buf, "AAAA") == 0);
        /* Size is the source's, not the longer victim's: a stale size would
         * mean the old inode is still behind the name. */
        struct stat st;
        ck("destination size is the source's (4), not the victim's (14)",
           stat("/rn_c.txt", &st) == 0 && st.st_size == 4);
    }

    CK_FAILS("rename of a missing source -> ENOENT", rename("/rn_nope", "/rn_x"), ENOENT);

    (void)unlink("/rn_c.txt");
}

static void test_signals(void) {
    printf("signals (self-delivery only -- nothing can interrupt from outside):\n");

    /* Never raise a SIG_DFL signal here: newlib routes it to kill(self), which
     * our libc implements as _exit(128+signo) -- it would end this test. */
    g_sig_seen = 0;
    void (*prev)(int) = signal(SIGUSR1, sig_handler);
    ck("signal() returns the previous disposition (SIG_DFL)", prev == SIG_DFL);

    ck("raise(SIGUSR1) == 0", raise(SIGUSR1) == 0);
    ck("handler RAN synchronously (real delivery for self)", g_sig_seen == SIGUSR1);

    /* POSIX-of-newlib: delivery resets the disposition to SIG_DFL. Worth
     * pinning -- a caller that re-arms is relying on it. */
    prev = signal(SIGUSR1, SIG_IGN);
    ck("disposition reset to SIG_DFL after delivery", prev == SIG_DFL);

    g_sig_seen = 0;
    ck("raise() while SIG_IGN == 0", raise(SIGUSR1) == 0);
    ck("ignored signal does NOT run a handler", g_sig_seen == 0);

    ck("kill(getpid(), 0) == 0 (existence probe, must not kill)",
       kill(getpid(), 0) == 0);

    /* The honest boundary: signalling ANOTHER process is not implemented, and
     * says so rather than pretending. */
    CK_FAILS("kill(other pid) -> ENOSYS (no cross-process delivery)",
             kill(getpid() + 1000, SIGTERM), ENOSYS);

    /* SIGPIPE is VACUOUSLY IGNORED here, and that happens to be what most
     * programs ask for anyway: our pipe write returns EPIPE (kernel ipc/pipe.c)
     * and no signal is ever raised -- exactly the behaviour signal(SIGPIPE,
     * SIG_IGN) buys on Unix. Recording the disposition is therefore truthful. */
    prev = signal(SIGPIPE, SIG_IGN);
    ck("signal(SIGPIPE, SIG_IGN) accepted (writes already return EPIPE)",
       prev != SIG_ERR);
}

int main(void) {
    printf("posixdemo: EmbLink POSIX layer self-check\n\n");

    test_tls();
    test_clocks();
    test_cwd();
    test_dir_basics();
    test_dir_errors();
    test_truncation_retry();
    test_relative_paths();
    test_file_read();
    test_large_file();
    test_widechar();
    test_fcntl();
    test_stat_access();
    test_sysparams();
    test_env();
    test_env_mutation();   /* AFTER test_env: it deliberately mutates the env */
    test_errno_translation();
    test_rename();
    test_signals();
    test_honest_refusals();

    printf("\nposixdemo: %s (%d failure%s)\n",
           failures == 0 ? "ALL PASS" : "FAILURES", failures,
           failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
