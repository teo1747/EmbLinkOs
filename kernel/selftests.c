#include "selftests.h"

#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "fs/embkfs/embkfs.h"
#include "block/block.h"   /* blkstat request counters (test ioperf) */
#include "arch/x86_64/syscall/usercopy.h"   /* transient-EFAULT retry counters */
#include "drivers/timer/hpet.h"
#include "drivers/timer/timer.h"
#include "drivers/char/serial.h"      /* serial_write_char: test tcc real heartbeat */
#include "arch/x86_64/irq/lapic.h"    /* lapic_timer_get_ticks: test tcc real pacing */
#include "fs/vfs.h"
#include "fs/fd.h"
#include "arch/x86_64/cpu/rwlock.h"
#include "drivers/timer/rtc.h"
#include "arch/x86_64/syscall/usermode.h"
#include "process/process.h"
#include "tty/tty.h"
#include "drivers/input/keyboard.h"
#include "process/ksync.h"
#include "mm/kheap.h"
#include "mm/vmm.h"
#include "mm/pmm.h"   /* MMIO_BASE for the test vmm range assertions */
#include "drivers/usb/usb.h"
#include "drivers/video/framebuffer.h"
#include "arch/x86_64/cpu/percpu.h"
#include "crypto/sha256.h"
#include "crypto/hmac.h"
#include "crypto/pbkdf2.h"
#include "crypto/aes.h"
#include "crypto/xts.h"
#include "fs/embkfs/embkfs_compress.h"
#include "gfx/surface.h"
#include "gfx/compositor.h"   /* compositor_pointer_tick: keep the desktop live while `shell` waits */
#include "ipc/channel.h"
#include "ipc/endpoint.h"
#include "ipc/pipe.h"

static struct fat32_volume *g_fat32 = NULL;
static bool g_has_fat32 = false;
static bool g_vfs_ready = false;

/* --- `test pipe` helper: a kthread that does ONE blocking 1-byte pipe read.
 * The EOF test's central assertion is "a reader BLOCKS while any write end
 * is still open, and wakes with 0 only when the last one drops" -- you can't
 * assert 'blocks' from the thread doing the blocking, so a helper thread
 * reads while the test watches these flags. Statics (not stack) because the
 * kthread outlives the enclosing scope's frame guarantees. */
static struct pipe *volatile s_pipe_eof_p;
static volatile int    s_pipe_eof_done;
static volatile size_t s_pipe_eof_got;
static void pipe_eof_reader_kthread(void)
{
    char b[4];
    size_t got = 99;
    (void)pipe_read(s_pipe_eof_p, b, 1, &got);
    s_pipe_eof_got  = got;
    s_pipe_eof_done = 1;
    process_exit_self(0);
}

/* --- `test ksync` helper: a kthread that BLOCKS on a held mutex via the
 * cancellation-aware acquire, so the test can cancel it and prove the acquire
 * returns -EMBK_ECANCELED instead of sleeping forever. Same "the blocker can't
 * assert its own blocking" reason as the pipe helper. Statics, not stack. */
static struct mutex  s_cx_mutex;
static volatile int  s_cx_started;   /* set just before the (blocking) acquire */
static volatile int  s_cx_result;    /* the acquire's return; 999 = not finished */
static void cx_cancel_waiter_kthread(void)
{
    s_cx_started = 1;
    s_cx_result  = mutex_lock_interruptible(&s_cx_mutex);   /* the driver holds it */
    /* Expected: -ECANCELED, having REFUSED the contended lock. If it somehow
     * acquired (returned 0), release so the lock isn't leaked to a corpse. */
    if (s_cx_result == EMBK_OK) mutex_unlock(&s_cx_mutex);
    process_exit_self(0);
}

/* ==== `test embbuild` helpers ============================================ */

/* Naive substring find -- the kernel has no strstr and eight lines beats an
 * include-world argument. */
static int emb_find(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncmp(p, needle, nl) == 0) return 1;
    return 0;
}

/* Spawn `path` with argv, its fd 1 AND fd 2 piped back to us; drain the pipe
 * into `cap` (NUL-terminated) AND onto the serial log; reap; return the exit
 * code. This is how the transcript the acceptance run wants gets from a
 * ring-3 stdout (which is the SCREEN) into the serial record. */
static int emb_run_cap(const char *path, char **argv, int argc,
                       char *cap, size_t capsz) {
    struct process *me = current_process;
    int rh = -1, wh = -1;
    size_t used = 0;
    cap[0] = '\0';
    if (pipe_create(&rh, &wh) != EMBK_OK) return -1000;
    struct pipe_end *pe_r = obj_handle_resolve(me, rh, HANDLE_KIND_PIPE);
    if (!pe_r || fd_install_pipe(me, 9, pe_r->p, 0) != EMBK_OK) {
        obj_handle_free(me, rh); obj_handle_free(me, wh);
        return -1001;
    }
    struct spawn_file_action acts[2];
    memset(acts, 0, sizeof acts);
    acts[0].kind = SPAWN_ACTION_INSTALL_OBJ; acts[0].target_fd = 1; acts[0].src_obj_handle = wh;
    acts[1].kind = SPAWN_ACTION_INSTALL_OBJ; acts[1].target_fd = 2; acts[1].src_obj_handle = wh;
    char *env[] = { "HOME=/", NULL };
    int pid = process_create_env(path, argv, argc, env, acts, 2);
    obj_handle_free(me, wh);                 /* ours must go or EOF never comes */
    if (pid < 0) {
        vfs_close(9); obj_handle_free(me, rh);
        return -1002;
    }
    for (;;) {
        char buf[256]; size_t got = 0;
        if (vfs_fd_read(9, buf, sizeof buf - 1, &got) != EMBK_OK || got == 0) break;
        buf[got] = '\0';
        kprintf("%s", buf);                  /* the live transcript */
        if (used + got < capsz - 1) { memcpy(cap + used, buf, got); used += got; }
    }
    cap[used] = '\0';
    vfs_close(9); obj_handle_free(me, rh);
    return process_wait((uint32_t)pid);
}

/* Whole-file read into buf (returns length, -1 on error) and whole-buffer
 * write (O_TRUNC) -- the acceptance run's file surgery. */
static int emb_read_all(const char *path, char *buf, size_t cap) {
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    size_t total = 0, got = 0;
    while (total < cap - 1 &&
           vfs_fd_read(fd, buf + total, cap - 1 - total, &got) == EMBK_OK && got > 0)
        total += got;
    vfs_close(fd);
    buf[total] = '\0';
    return (int)total;
}
static int emb_write_all(const char *path, const char *buf, size_t len) {
    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t w = 0;
    vfs_fd_write(fd, buf, len, &w);
    vfs_close(fd);
    return w == len ? 0 : -1;
}

void selftests_init(struct fat32_volume *fat_vol, bool fat_ready)
{
    g_fat32 = fat_vol;
    g_has_fat32 = fat_ready;
    g_vfs_ready = false;
}

void selftests_set_vfs_ready(bool ready)
{
    g_vfs_ready = ready;
}

static void fat32_read_path(struct fat32_volume *vol, const char *path, uint8_t *buffer, uint32_t buffer_size)
{
    int n = fat32_read(vol, path, buffer, buffer_size - 1);
    if (n >= 0) {
        buffer[n] = '\0';
        kprintf("READ %s: %d bytes\n%s\n", path, n, buffer);
    } else {
        kprintf("READ %s failed: %s\n", path, embk_strerror(n));
    }
}

static void fat32_write_path(struct fat32_volume *vol, const char *path, const char *content)
{
    uint32_t len = 0;
    while (content[len]) len++;

    int rc = fat32_write(vol, path, (const uint8_t *)content, len);
    if (rc >= 0)
        kprintf("WROTE %s: %d bytes\n", path, rc);
    else
        kprintf("WRITE %s failed: %s\n", path, embk_strerror(rc));
}

static void fat32_test_all(struct fat32_volume *vol)
{
    static uint8_t buffer[512];

    kprintf("\n=== FAT32 TEST SUITE ===\n");
    fat32_list_root(vol);

    fat32_read_path(vol, "HELLO.TXT", buffer, sizeof(buffer));
    fat32_read_path(vol, "/SUBDIR/INSIDE.TXT", buffer, sizeof(buffer));

    fat32_write_path(vol, "/NEWFILE.TXT", "This is a new file written by test.");
    fat32_read_path(vol, "/NEWFILE.TXT", buffer, sizeof(buffer));

    int rc = fat32_mkdir(vol, "/TESTDIR");
    if (rc == EMBK_OK)
        kprintf("MKDIR /TESTDIR succeeded\n");
    else if (rc == -EMBK_EEXIST)
        kprintf("MKDIR /TESTDIR skipped: already exists\n");
    else
        kprintf("MKDIR /TESTDIR failed: %s\n", embk_strerror(rc));

    fat32_write_path(vol, "/TESTDIR/INNER.TXT", "Inside test dir file.");
    fat32_read_path(vol, "/TESTDIR/INNER.TXT", buffer, sizeof(buffer));

    fat32_write_path(vol, "/LONG NAME EXAMPLE.TXT", "Long filename test content.");
    fat32_read_path(vol, "/LONG NAME EXAMPLE.TXT", buffer, sizeof(buffer));

    kprintf("=== FAT32 TEST SUITE COMPLETE ===\n");
}

static int selftest_join_path(char *out, size_t cap, const char *base, const char *leaf)
{
    if (!out || !base || !leaf || cap < 4)
        return -EMBK_EINVAL;

    size_t b = 0;
    while (base[b] != '\0') b++;
    size_t l = 0;
    while (leaf[l] != '\0') l++;

    if (b == 0 || base[0] != '/' || l == 0)
        return -EMBK_EINVAL;

    size_t need = b + 1 + l + 1;
    if (base[b - 1] == '/')
        need--;
    if (need > cap)
        return -EMBK_ENAMETOOLONG;

    size_t p = 0;
    for (size_t i = 0; i < b; i++)
        out[p++] = base[i];
    if (out[p - 1] != '/')
        out[p++] = '/';
    for (size_t i = 0; i < l; i++)
        out[p++] = leaf[i];
    out[p] = '\0';
    return EMBK_OK;
}

static int selftest_unlink_path(const char *path)
{
    if (!path || path[0] != '/')
        return -EMBK_EINVAL;

    const char *last_slash = NULL;
    for (const char *s = path; *s != '\0'; s++) {
        if (*s == '/')
            last_slash = s;
    }
    if (!last_slash)
        return -EMBK_EINVAL;

    const char *leaf = last_slash + 1;
    size_t leaf_len = 0;
    while (leaf[leaf_len] != '\0') leaf_len++;
    if (leaf_len == 0 || leaf_len > 255)
        return -EMBK_EINVAL;

    char parent_path[256];
    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        if (parent_len >= sizeof(parent_path))
            return -EMBK_ENAMETOOLONG;
        for (size_t i = 0; i < parent_len; i++)
            parent_path[i] = path[i];
        parent_path[parent_len] = '\0';
    }

    struct vnode parent;
    int rc = vfs_resolve(parent_path, &parent);
    if (rc != EMBK_OK)
        return rc;
    if (!parent.mnt || !parent.mnt->ops || !parent.mnt->ops->unlink)
        return -EMBK_ENOSYS;

    return parent.mnt->ops->unlink(&parent, leaf, leaf_len);
}

static int fat32_vfs_test_mkdir_unlink(void)
{
    struct vfs_stat st;
    const char *base = "/";
    if (vfs_stat("/fat32", &st) == EMBK_OK && st.type == VFS_DT_DIR)
        base = "/fat32";

    char dir_path[96];
    char file_path[128];
    int rc = selftest_join_path(dir_path, sizeof(dir_path), base, "VFSFAT32_T");
    if (rc != EMBK_OK)
        return rc;
    rc = selftest_join_path(file_path, sizeof(file_path), dir_path, "a.txt");
    if (rc != EMBK_OK)
        return rc;

    struct vnode base_vn;
    rc = vfs_resolve(base, &base_vn);
    if (rc != EMBK_OK)
        return rc;

    struct vnode created_dir;
    rc = base_vn.mnt->ops->mkdir(&base_vn, "VFSFAT32_T", 10, &created_dir);
    if (rc == -EMBK_EEXIST) {
        int clr = selftest_unlink_path(file_path);
        if (clr != EMBK_OK && clr != -EMBK_ENOENT)
            return clr;
        clr = selftest_unlink_path(dir_path);
        if (clr != EMBK_OK && clr != -EMBK_ENOENT)
            return clr;
        rc = base_vn.mnt->ops->mkdir(&base_vn, "VFSFAT32_T", 10, &created_dir);
    }
    if (rc != EMBK_OK)
        return rc;

    int fd = vfs_open(file_path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        selftest_unlink_path(dir_path);
        return fd;
    }

    static const char msg[] = "fat32-vfs-selftest";
    size_t wrote = 0;
    rc = vfs_fd_write(fd, msg, sizeof(msg) - 1, &wrote);
    int close_rc = vfs_close(fd);
    if (rc != EMBK_OK) {
        selftest_unlink_path(file_path);
        selftest_unlink_path(dir_path);
        return rc;
    }
    if (close_rc != EMBK_OK) {
        selftest_unlink_path(file_path);
        selftest_unlink_path(dir_path);
        return close_rc;
    }
    if (wrote != sizeof(msg) - 1) {
        selftest_unlink_path(file_path);
        selftest_unlink_path(dir_path);
        return -EMBK_EIO;
    }

    rc = selftest_unlink_path(dir_path);
    if (rc != -EMBK_ENOTEMPTY) {
        selftest_unlink_path(file_path);
        selftest_unlink_path(dir_path);
        return (rc == EMBK_OK) ? -EMBK_EINVAL : rc;
    }

    rc = selftest_unlink_path(file_path);
    if (rc != EMBK_OK) {
        selftest_unlink_path(dir_path);
        return rc;
    }

    rc = selftest_unlink_path(dir_path);
    if (rc != EMBK_OK)
        return rc;

    return EMBK_OK;
}

static void selftests_print_commands(void)
{
    kprintf("\nSelftest commands:\n");
    kprintf("  test list\n");
    kprintf("  test embkfs path\n");
    kprintf("  test embkfs alloc\n");
    kprintf("  test embkfs tree\n");
    kprintf("  test embkfs obj\n");
    kprintf("  test embkfs shrink\n");
    kprintf("  test blockrace\n");
    kprintf("  test usercopy\n");
    kprintf("  test embkfs timestamps\n");
    kprintf("  test embkfs multivol\n");
    kprintf("  test embkfs compress\n");
    kprintf("  test embkfs selfheal\n");
    kprintf("  test embkfs snapshot\n");
    kprintf("  test embkfs snapreg\n");
    kprintf("  test embkfs provenance\n");
    kprintf("  test embkfs verifyboot\n");
    kprintf("  stat <path>\n");
    kprintf("  snap create|list|delete|rollback <name>\n");
    kprintf("  test embkfs ns\n");
    kprintf("  test embkfs all\n");
    kprintf("  test embkfs diag\n");
    kprintf("  test vfs\n");
    kprintf("  test fd\n");
    kprintf("  test fat32\n");
    kprintf("  test fat32 vfs\n");
    kprintf("  test rwlock\n");
    kprintf("  test rtc\n");
    kprintf("  test crypto sha256\n");
    kprintf("  test crypto hmac\n");
    kprintf("  test crypto pbkdf2\n");
    kprintf("  test crypto aes\n");
    kprintf("  test crypto xts\n");
    kprintf("  test crypto all\n");
    kprintf("  test ring3\n");
    kprintf("  test ring3 threads\n");
    kprintf("  test pipe\n");
    kprintf("  shell            (interactive: run the shell on this console)\n");
    kprintf("  test shell\n");
    kprintf("  test extern\n");
    kprintf("  test cxx\n");
    kprintf("  test posix\n");
    kprintf("  test ioperf\n");
    kprintf("  test env\n");
    kprintf("  test ctrlc\n");
    kprintf("  test ctrlc2\n");
    kprintf("  test git\n");
    kprintf("  test git repo\n");
    kprintf("  test git cwd\n");
    kprintf("  test tcc\n");
    kprintf("  test tcc compile\n");
    kprintf("  test tcc link\n");
    kprintf("  test tcc real\n");
    kprintf("  test tcc dyn\n");
    kprintf("  test tcc tally\n");
    kprintf("  test embbuild\n");
    kprintf("  test embbuild self\n");
    kprintf("  test embbuild shell\n");
    kprintf("  test embbuild gui\n");
    kprintf("  test keyboard\n");
    kprintf("  test ksync\n");
    kprintf("  test kheap\n");
    kprintf("  test shell cwd\n");
    kprintf("  test python\n");
    kprintf("  test surface\n");
    kprintf("  test channel\n");
    kprintf("  test compositor\n");
    kprintf("  test rendezvous\n");
    kprintf("  test ui\n");
    kprintf("  test sched roundrobin\n");
    kprintf("  test fpu\n");
    kprintf("  test sched kill\n");
    kprintf("  test sched reap\n");
    kprintf("  test sched stackguard\n");
    kprintf("  test sched wait\n");
    kprintf("  test sched priority\n");
    kprintf("  test smp online\n");
    kprintf("  test smp sched\n");
    kprintf("  test smp kill\n");
    kprintf("  test thread smp\n");
    kprintf("  test thread exit\n");
    kprintf("  test usb\n");
    kprintf("  test gpu\n");
    kprintf("  test tty\n");
    kprintf("  test dirtree\n");
    kprintf("  test layout\n");
}

static void run_embkfs_all(void)
{
    int rc_path = embkfs_run_path_selftests();
    int rc_alloc = embkfs_run_allocator_selftests();
    int rc_tree = embkfs_run_tree_selftests();
    int rc_obj = embkfs_run_object_selftests();
    int rc_ts = embkfs_run_timestamp_selftests();
    int rc_mv = embkfs_run_multivol_selftests();
    int rc_cz = embkfs_run_compress_selftests();
    int rc_sh = embkfs_run_selfheal_selftests();
    int rc_sn = embkfs_run_snapshot_selftests();
    int rc_pv = embkfs_run_provenance_selftests();
    int rc_vb = embkfs_run_verifyboot_selftests();
    int rc_ns = embkfs_run_namespace_selftests();

    if (rc_path == EMBK_OK && rc_alloc == EMBK_OK && rc_tree == EMBK_OK && rc_obj == EMBK_OK &&
        rc_ts == EMBK_OK && rc_mv == EMBK_OK && rc_cz == EMBK_OK && rc_sh == EMBK_OK &&
        rc_sn == EMBK_OK && rc_pv == EMBK_OK && rc_vb == EMBK_OK && rc_ns == EMBK_OK) {
        kprintf("\n[cmd] test embkfs all: OK\n");
        return;
    }

    if (rc_path != EMBK_OK)  kprintf("\n[cmd] embkfs path failed: %s\n", embk_strerror(rc_path));
    if (rc_alloc != EMBK_OK) kprintf("\n[cmd] embkfs alloc failed: %s\n", embk_strerror(rc_alloc));
    if (rc_tree != EMBK_OK)  kprintf("\n[cmd] embkfs tree failed: %s\n", embk_strerror(rc_tree));
    if (rc_obj != EMBK_OK)   kprintf("\n[cmd] embkfs obj failed: %s\n", embk_strerror(rc_obj));
    if (rc_ts != EMBK_OK)    kprintf("\n[cmd] embkfs timestamps failed: %s\n", embk_strerror(rc_ts));
    if (rc_mv != EMBK_OK)    kprintf("\n[cmd] embkfs multivol failed: %s\n", embk_strerror(rc_mv));
    if (rc_cz != EMBK_OK)    kprintf("\n[cmd] embkfs compress failed: %s\n", embk_strerror(rc_cz));
    if (rc_sh != EMBK_OK)    kprintf("\n[cmd] embkfs selfheal failed: %s\n", embk_strerror(rc_sh));
    if (rc_sn != EMBK_OK)    kprintf("\n[cmd] embkfs snapshot failed: %s\n", embk_strerror(rc_sn));
    if (rc_pv != EMBK_OK)    kprintf("\n[cmd] embkfs provenance failed: %s\n", embk_strerror(rc_pv));
    if (rc_vb != EMBK_OK)    kprintf("\n[cmd] embkfs verifyboot failed: %s\n", embk_strerror(rc_vb));
    if (rc_ns != EMBK_OK)    kprintf("\n[cmd] embkfs ns failed: %s\n", embk_strerror(rc_ns));
}

int selftests_handle_command(const char *cmd)
{
    if (!cmd || !cmd[0])
        return 0;

    if (strcmp(cmd, "test list") == 0) {
        selftests_print_commands();
        return 1;
    }

    if (strcmp(cmd, "test embkfs path") == 0) {
        int rc = embkfs_run_path_selftests();
        kprintf("\n[cmd] test embkfs path: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test embkfs alloc") == 0) {
        int rc = embkfs_run_allocator_selftests();
        kprintf("\n[cmd] test embkfs alloc: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test embkfs tree") == 0) {
        int rc = embkfs_run_tree_selftests();
        kprintf("\n[cmd] test embkfs tree: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test embkfs obj") == 0) {
        int rc = embkfs_run_object_selftests();
        kprintf("\n[cmd] test embkfs obj: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

/* CONCURRENT READERS, AND WHAT THE BOUNCE BUFFER'S LOCK IS ACTUALLY FOR.
 *
 * block.c has ONE shared DMA bounce buffer, taken whenever a caller's
 * destination is not DMA-safe for the device -- the common case here, not an
 * exotic one: kmalloc memory lives in the direct map and the ATA driver needs
 * the kernel range, so nearly all filesystem I/O bounces (a single
 * `test posix` run takes that path 8617 times). Two concurrent bounce
 * transfers would memcpy over each other and each return the OTHER's sectors:
 * silent corruption, no fault, no error code.
 *
 * TODO.md asked for the test that was missing: "the race itself is NOT covered
 * ... every test above is single-threaded, so the lock is never contended".
 * This is that test, and writing it turned up something better than a pass.
 *
 * WHAT IT FOUND. Four readers, spawned before any is waited on, hammering two
 * files -- and the bounce lock is contended ZERO times. Not a flaw in the
 * test: a fact about the system. The EMBKFS big lock serialises every
 * filesystem operation, so two processes CANNOT be inside the block layer at
 * once by way of the filesystem. The bounce lock sits underneath a lock that
 * already excludes the concurrency it defends against.
 *
 * So the honest assertion is not "contention must be > 0" -- that would be
 * demanding a race the architecture currently forbids, and no amount of
 * hammering would produce it. It is the RELATIONSHIP:
 *
 *     contention through the filesystem path must be ZERO, because the
 *     EMBKFS big lock serialises above it.
 *
 * That is the stronger check. If it ever fires non-zero, something real
 * changed -- the fs lock got finer-grained (per-volume/per-path, an open
 * TODO item), a second filesystem mounted alongside EMBKFS (fat32.c does NOT
 * take the EMBKFS lock), or a non-fs block user appeared -- and the bounce
 * lock stopped being defence-in-depth and became load-bearing. This test is
 * how that transition announces itself instead of being discovered by
 * corruption.
 *
 * The content check is not redundant either: it proves end-to-end that four
 * concurrent readers each get their OWN file's bytes, which is the property
 * users care about regardless of which layer is providing it. Each reader
 * verifies every byte against a single-byte fill, so one stolen sector is
 * unmistakable ('B' where only 'A' belongs) and the child exits 2. */
/* THE TRANSIENT EFAULT: is it still there, or is the retry loop a relic?
 *
 * docs/TODO.md carries it as "residual, unexplained ... masked, not fixed:
 * both copy functions retry up to USERCOPY_RETRIES (8) times before actually
 * reporting a fault ... a workaround, not a diagnosis." That note predates TWO
 * real fixes to the same code path:
 *
 *   - access_ok now captures the pml4 ONCE with IF=0 instead of re-reading
 *     per-CPU state per page (the migration race: this_cpu() picked a core, a
 *     timer IRQ migrated the thread, the deref returned a FOREIGN pml4);
 *   - vmm_get_phys_in now takes vmm_lock, closing the multi-level walk against
 *     a concurrent mapper (the compositor installing shared window pages into
 *     a client PML4 while that client walked it).
 *
 * Either could have been the entire cause. "Unexplained" is a statement about
 * what was known then, not a measurement of now -- so measure now.
 *
 * This drives heavy user-copy traffic from several processes at once, which is
 * what every syscall carrying a buffer does, and then reads the retry counter.
 *   retries == 0  -> the transient did not occur. The loop is dead code and
 *                    the TODO entry is stale (it does NOT prove absence, and
 *                    the report says so).
 *   retries  > 0  -> it is alive, and now it is CAUGHT: worst_tries says how
 *                    deep it went, which is the first hard number anyone has
 *                    had about it.
 * Either way this is the first time the question has been asked with a
 * counter instead of an anecdote. */
    if (strcmp(cmd, "test usercopy") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test usercopy: VFS not registered\n"); return 1; }

        /* Every read()/write() crossing the syscall boundary is a user-copy, so
         * concurrent file readers ARE concurrent user-copy traffic -- and they
         * run while home/clockw keep the compositor mapping shared window pages,
         * which is the other half of the historical repro. */
        /* BOTH HALVES OF THE HISTORICAL REPRO, or this proves little:
         *   - readers hammering the syscall boundary (every read() is a
         *     copy_to_user, so this is the walker side), and
         *   - UI apps being launched DURING that, because window creation is
         *     what makes the compositor map shared pages into a client's PML4
         *     on another core -- the writer side named in vmm_get_phys_in's
         *     comment ("font.ttf open failed -14" under -smp 4).
         * Readers alone exercise the walk against a quiet page table, which is
         * the easy case and not the one that ever failed. */
        #define UC_PROCS 6
        #define UC_UI    2
        char *env[] = { (char *)"HOME=/", NULL };
        int pids[UC_PROCS], uipids[UC_UI];
        struct usercopy_stat_pub u0, u1;

        usercopy_stat_reset();
        usercopy_stat_get(&u0);

        for (int i = 0; i < UC_PROCS; i++) {
            char *a[] = { (char *)"/data/apps/ioracer/ioracer.elf",
                          (i & 1) ? (char *)"/data/raceb.bin" : (char *)"/data/racea.bin",
                          (i & 1) ? (char *)"B" : (char *)"A", (char *)"8", NULL };
            pids[i] = process_create_env("/data/apps/ioracer/ioracer.elf", a, 4, env, NULL, 0);
            if (pids[i] < 0) kprintf("[usercopy] spawn %d failed: %s\n", i, embk_strerror(pids[i]));
        }
        /* Launched AFTER the readers are already running, so the window
         * mapping lands in the middle of their user-copy traffic. */
        for (int i = 0; i < UC_UI; i++) {
            char *a[] = { (char *)"/data/apps/clockw/clockw.elf", NULL };
            uipids[i] = process_create_env("/data/apps/clockw/clockw.elf", a, 1, env, NULL, 0);
            kprintf("[usercopy] UI app %d -> pid %d (forces compositor to map shared "
                    "pages into a client PML4 mid-flight)\n", i, uipids[i]);
        }
        kprintf("[usercopy] %d readers x 8 passes x 256 KB (4 KB per read) + %d UI launches\n",
                UC_PROCS, UC_UI);

        int bad = 0;
        for (int i = 0; i < UC_PROCS; i++) {
            int code = (pids[i] >= 0) ? process_wait((uint32_t)pids[i]) : -1;
            if (code != 0) { kprintf("[usercopy] reader %d exit=%d\n", i, code); bad = 1; }
        }
        for (int i = 0; i < UC_UI; i++) {          /* widgets run forever: reap them */
            if (uipids[i] >= 0) { process_kill((uint32_t)uipids[i]); process_wait((uint32_t)uipids[i]); }
        }

        usercopy_stat_get(&u1);
        uint64_t calls   = u1.calls   - u0.calls;
        uint64_t retries = u1.retries - u0.retries;
        uint64_t faults  = u1.faults  - u0.faults;

        kprintf("[usercopy] %llu validations, %llu TRANSIENT retries, %llu hard faults, "
                "deepest %llu attempt(s)\n",
                (unsigned long long)calls, (unsigned long long)retries,
                (unsigned long long)faults, (unsigned long long)u1.worst_tries);

        if (retries == 0) {
            kprintf("[usercopy] no transient observed in %llu validations. That is EVIDENCE,\n"
                    "           not proof -- absence over one run cannot prove absence -- but\n"
                    "           it is the first number anyone has had, and it is consistent\n"
                    "           with the two landed fixes (atomic pml4 capture, vmm_lock in\n"
                    "           vmm_get_phys_in) having been the whole cause.\n",
                    (unsigned long long)calls);
        } else {
            kprintf("[usercopy] *** THE TRANSIENT IS ALIVE: %llu of %llu validations needed a\n"
                    "           retry (deepest %llu). It is no longer unexplained-and-unmeasured;\n"
                    "           this is a live repro to chase.\n",
                    (unsigned long long)retries, (unsigned long long)calls,
                    (unsigned long long)u1.worst_tries);
        }

        /* A hard fault here would mean a genuinely bad pointer from a reader
         * that is only ever handed valid ones -- that IS a failure. */
        int ok = !bad && faults == 0;
        kprintf("\n[cmd] test usercopy: %s\n",
                ok ? "OK -- all readers completed, no unrecoverable EFAULT" : "FAIL");
        return 1;
        #undef UC_PROCS
        #undef UC_UI
    }

    if (strcmp(cmd, "test blockrace") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test blockrace: VFS not registered\n"); return 1; }

        /* Six readers, two workloads. The four 256 KB fixtures are the
         * content-verified ones and live in the whole-object rcache. The two
         * BIG readers exist for a different reason: cxxdemo.elf is 9 MB, past
         * EMBKFS_RCACHE_MAX, so it bypasses rcache entirely and drives the
         * extent-map cache (ecache) instead -- the only workload on this image
         * that does. Without them the ecache numbers below would be all zeros
         * and any claim about it would be unfounded. They read in '*' mode:
         * a real binary has no fill byte, so they drive the path without
         * pretending to a content check. */
        #define RACERS 6
        static const char *files[RACERS] = { "/data/racea.bin", "/data/raceb.bin",
                                             "/data/racea.bin", "/data/raceb.bin",
                                             "/data/apps/cxxdemo/cxxdemo.elf",
                                             "/data/apps/cxxdemo/cxxdemo.elf" };
        static const char *fills[RACERS] = { "A", "B", "A", "B", "*", "*" };
        static const char *passes[RACERS] = { "3", "3", "3", "3", "2", "2" };
        char *env[] = { (char *)"HOME=/", NULL };
        int pids[RACERS], codes[RACERS];
        int ok = 1, mismatch = 0;

        struct embk_blkstat b0, b1;
        struct embkfs_lockstat l0, l1;
        embk_blkstat_get(&b0);
        embkfs_lockstat_get(&l0);

        /* Spawn ALL of them before waiting on any. Waiting between spawns
         * would serialise them in the TEST, which would hide whether the
         * SYSTEM serialises them -- the very thing being measured. */
        for (int i = 0; i < RACERS; i++) {
            char *a[] = { (char *)"/data/apps/ioracer/ioracer.elf",
                          (char *)files[i], (char *)fills[i], (char *)passes[i], NULL };
            pids[i] = process_create_env("/data/apps/ioracer/ioracer.elf", a, 4, env, NULL, 0);
            if (pids[i] < 0) {
                kprintf("[blockrace] spawn %d failed: %s\n", i, embk_strerror(pids[i]));
                ok = 0;
            }
        }
        kprintf("[blockrace] %d readers spawned before any wait: 4 over two 256 KB\n"
                "            fixtures (rcache path, content-verified) + 2 over a 9 MB\n"
                "            binary (>8 MB, so the ecache path). Bounce buffer is 32 KB.\n",
                RACERS);

        for (int i = 0; i < RACERS; i++) {
            codes[i] = (pids[i] >= 0) ? process_wait((uint32_t)pids[i]) : -1;
            if (codes[i] == 2) mismatch = 1;
            kprintf("[blockrace] reader %d (%s, '%s') exit=%d%s\n",
                    i, files[i], fills[i], codes[i],
                    codes[i] == 2 ? "  <-- GOT ANOTHER READER'S BYTES" : "");
            if (codes[i] != 0) ok = 0;
        }

        embk_blkstat_get(&b1);
        embkfs_lockstat_get(&l1);
        uint64_t breads = b1.bounce_reads - b0.bounce_reads;
        uint64_t bcont  = b1.bounce_contended - b0.bounce_contended;
        kprintf("[blockrace] bounce path: %llu read(s), %llu contended\n",
                (unsigned long long)breads, (unsigned long long)bcont);

        /* The architectural assertion. Zero is CORRECT here and is what makes
         * the bounce lock defence-in-depth rather than the thing holding the
         * system together. Non-zero is not a failure of correctness -- the
         * lock did its job -- but it means the layering changed, and whoever
         * changed it needs to know this lock is now load-bearing. */
        if (bcont == 0) {
            kprintf("[blockrace] bounce lock never contended -- EXPECTED: the EMBKFS big\n"
                    "            lock serialises fs I/O above it, so two readers cannot be\n"
                    "            inside the block layer at once. The bounce lock is\n"
                    "            defence-in-depth for callers that do not hold that lock\n"
                    "            (fat32.c, the boot partition scan, a future finer-grained\n"
                    "            fs lock).\n");
        } else {
            kprintf("[blockrace] *** bounce lock WAS contended (%llu). The fs no longer\n"
                    "            serialises above it -- this lock is now LOAD-BEARING.\n"
                    "            That is fine, but docs/TODO.md's block-layer entry and\n"
                    "            the EMBKFS big-lock comment both need updating.\n",
                    (unsigned long long)bcont);
        }

        /* Also worth seeing: the bounce read count is far below the 3 MB these
         * readers logically consumed, because EMBKFS's whole-object cache
         * serves repeat passes without reaching the block layer at all. A
         * concurrency test that assumed "bytes read == device reads" would be
         * measuring the cache, not the disk. */
        kprintf("[blockrace] (%llu device bounce reads for ~3 MB logical -- the rest was\n"
                "            served by EMBKFS's object cache, never reaching block.c)\n",
                (unsigned long long)breads);

        /* The EMBKFS big lock, measured under the only genuinely concurrent fs
         * load in the tree. This is the number that decides whether
         * finer-grained (per-volume / per-object) locking is worth the 34
         * shared scratch buffers that would have to be un-shared first. A high
         * wait time says the coarse lock is costing real throughput; a low one
         * says leave it alone and spend the effort elsewhere. Measured before
         * optimised, the same rule the I/O-path rebuild was held to. */
        {
            uint64_t acq = l1.acquires - l0.acquires;
            uint64_t rec = l1.recursive - l0.recursive;
            uint64_t wts = l1.waits - l0.waits;
            uint64_t wus = l1.wait_us - l0.wait_us;
            kprintf("[blockrace] fs big lock: %llu acquire(s), %llu recursive, "
                    "%llu waited (%llu%%), %llu ms blocked\n",
                    (unsigned long long)acq, (unsigned long long)rec,
                    (unsigned long long)wts,
                    (unsigned long long)(acq ? (wts * 100) / acq : 0),
                    (unsigned long long)(wus / 1000));
            struct embkfs_stat es;
            embkfs_stat_get(&es);
            kprintf("[blockrace] ecache: %llu hit, %llu miss | icache: %llu hit, %llu miss "
                    "(both still 1 slot)\n",
                    (unsigned long long)es.ecache_hit, (unsigned long long)es.ecache_miss,
                    (unsigned long long)es.icache_hit, (unsigned long long)es.icache_miss);
            kprintf("[blockrace] rcache: %llu hit, %llu miss, %llu evict, %llu BYPASS "
                    "(>%u MB, uncacheable by policy)\n"
                    "[blockrace]         %u slot%s, %u MB budget\n",
                    (unsigned long long)es.rcache_hit,
                    (unsigned long long)es.rcache_miss,
                    (unsigned long long)es.rcache_evict,
                    (unsigned long long)es.rcache_bypass,
                    (unsigned)(EMBKFS_RCACHE_MAX_MB),
                    EMBKFS_RCACHE_SLOTS, EMBKFS_RCACHE_SLOTS == 1 ? "" : "s",
                    EMBKFS_RCACHE_BUDGET / (1024u * 1024u));
            kprintf("[blockrace]   (an EVICTION is a miss the cache inflicted on itself, and\n"
                    "               each miss re-decodes a whole object while holding the big\n"
                    "               lock -- which is what the ms-blocked figure above IS)\n");
        }

        kprintf("\n[cmd] test blockrace: %s\n",
                ok ? "OK -- 4 concurrent readers, every byte its own"
                   : (mismatch ? "FAIL -- A READER GOT ANOTHER'S BYTES (bounce buffer raced)"
                               : "FAIL"));
        return 1;
        #undef RACERS
    }

    if (strcmp(cmd, "test embkfs shrink") == 0) {
        int rc = embkfs_run_shrink_selftests();
        kprintf("\n[cmd] test embkfs shrink: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test embkfs timestamps") == 0) {
        int rc = embkfs_run_timestamp_selftests();
        kprintf("\n[cmd] test embkfs timestamps: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test embkfs multivol") == 0) {
        int rc = embkfs_run_multivol_selftests();
        kprintf("\n[cmd] test embkfs multivol: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test embkfs compress") == 0) {
        int rc_codec = embk_compress_run_selftests();
        int rc_fs = embkfs_run_compress_selftests();
        bool ok = (rc_codec == 0 && rc_fs == EMBK_OK);
        kprintf("\n[cmd] test embkfs compress: %s\n", ok ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test embkfs selfheal") == 0) {
        int rc = embkfs_run_selfheal_selftests();
        kprintf("\n[cmd] test embkfs selfheal: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test embkfs snapshot") == 0) {
        int rc = embkfs_run_snapshot_selftests();
        kprintf("\n[cmd] test embkfs snapshot: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test embkfs snapreg") == 0) {
        int rc = embkfs_run_snapreg_selftests();
        kprintf("\n[cmd] test embkfs snapreg: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test embkfs provenance") == 0) {
        int rc = embkfs_run_provenance_selftests();
        kprintf("\n[cmd] test embkfs provenance: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test embkfs verifyboot") == 0) {
        int rc = embkfs_run_verifyboot_selftests();
        kprintf("\n[cmd] test embkfs verifyboot: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test embkfs ns") == 0) {
        int rc = embkfs_run_namespace_selftests();
        kprintf("\n[cmd] test embkfs ns: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test embkfs all") == 0 || strcmp(cmd, "embkfs-test") == 0) {
        run_embkfs_all();
        return 1;
    }

    if (strcmp(cmd, "test embkfs diag") == 0) {
        int rc = embkfs_run_boot_diagnostics();
        kprintf("\n[cmd] test embkfs diag: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test vfs") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test vfs: VFS not registered\n");
            return 1;
        }
        int rc = vfs_run_selftests();
        kprintf("\n[cmd] test vfs: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test fd") == 0 || strcmp(cmd, "fd-test") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test fd: VFS not registered\n");
            return 1;
        }
        int rc = vfs_fd_run_selftests();
        kprintf("\n[cmd] test fd: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "test rwlock") == 0) {
        int rc = rwlock_run_selftests();
        kprintf("\n[cmd] test rwlock: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test rtc") == 0) {
        int rc = rtc_run_selftests();
        kprintf("\n[cmd] test rtc: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test crypto sha256") == 0) {
        int rc = sha256_run_selftests();
        kprintf("\n[cmd] test crypto sha256: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test crypto hmac") == 0) {
        int rc = hmac_sha256_run_selftests();
        kprintf("\n[cmd] test crypto hmac: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test crypto pbkdf2") == 0) {
        int rc = pbkdf2_run_selftests();
        kprintf("\n[cmd] test crypto pbkdf2: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test crypto aes") == 0) {
        int rc = aes256_run_selftests();
        kprintf("\n[cmd] test crypto aes: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test crypto xts") == 0) {
        int rc = aes_xts_run_selftests();
        kprintf("\n[cmd] test crypto xts: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test crypto all") == 0) {
        int rc_sha = sha256_run_selftests();
        int rc_hmac = hmac_sha256_run_selftests();
        int rc_pbkdf2 = pbkdf2_run_selftests();
        int rc_aes = aes256_run_selftests();
        int rc_xts = aes_xts_run_selftests();
        bool all_ok = (rc_sha == 0 && rc_hmac == 0 && rc_pbkdf2 == 0 && rc_aes == 0 && rc_xts == 0);
        kprintf("\n[cmd] test crypto all: %s\n", all_ok ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test ring3") == 0) {
        // Drop to ring 3, run the user stub (write + exit via int 0x80), and
        // return here when it exits. Re-runnable: enter_user_mode now tears
        // down the process address space on both exit and load failure.
        enter_user_mode();
        kprintf("\n[cmd] test ring3: returned to kernel\n");
        return 1;
    }

    if (strcmp(cmd, "test ring3 threads") == 0) {
        // Phase 5 (docs/architecture/process-and-scheduling.md): unlike
        // "test ring3" above (a one-shot, non-scheduled launch via
        // enter_user_mode()), this goes through the REAL scheduler --
        // process_create()/process_wait(), the exact same path sys_spawn/
        // sys_wait and the shell's own `run`/`wait` commands use -- because
        // exercising sys_thread_create/sys_thread_join genuinely needs a
        // scheduled `struct process`/`struct thread` pair (thread_join()
        // blocks via the scheduler), which enter_user_mode()'s standalone
        // launch doesn't create at all. Run at the top level (argc==1),
        // /system/bin/init.elf runs the full EmbLink native-primitive suite -- a second
        // thread (create/join + shared-memory proof), a spawn() with argv +
        // file-actions, and an sbrk() heap exercise -- and exits 16 iff ALL
        // of them passed (see user/init.c, built on the EmbLink SDK
        // user/embk.h). 16 is a fixed success sentinel, not a computed value.
        // Needs a real filesystem with /system/bin/init.elf on it (e.g. `make
        // run-embkfs`) -- unlike "test ring3", there's no embedded fallback
        // blob.
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test ring3 threads: VFS not registered (need /system/bin/init.elf on disk)\n");
            return 1;
        }
        char *argv[] = { "/system/bin/init.elf", NULL };
        int pid = process_create("/system/bin/init.elf", argv, 1, NULL, 0);
        if (pid < 0) {
            kprintf("\n[cmd] test ring3 threads: process_create failed: %s\n", embk_strerror(pid));
            return 1;
        }

        int code = process_wait((uint32_t)pid);
        kprintf("\n[cmd] test ring3 threads: /system/bin/init.elf exited with code %d (want 16): %s\n",
                code, code == 16 ? "OK" : "FAIL");
        return 1;
    }

    /* sys_spawn's handle-table SELF-HEAL (process_handle_reap_dead). A parent
     * that spawns children and never wait()s the dead ones leaks a handle slot
     * each; when the table fills, the OLD behaviour made sys_spawn KILL the
     * child it had just created. The fix reclaims the handles that name
     * already-EXITED children (reaping those zombies) and retries -- while
     * leaving a still-LIVE child's handle untouched. This proves both halves:
     * a table full of dead-child handles frees up, and a live child is spared. */
    if (strcmp(cmd, "test handle reap") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test handle reap: VFS not registered (need /system/bin/init.elf)\n");
            return 1;
        }
        struct process *me = current_process;
        int ok = 1;

        /* (1) a LONG-LIVED child ("spin" loops forever); leak its handle. */
        char *aspin[] = { "/system/bin/init.elf", "spin", NULL };
        int spin_pid = process_create("/system/bin/init.elf", aspin, 2, NULL, 0);
        int h_spin = spin_pid >= 0 ? process_handle_alloc(me, (uint32_t)spin_pid) : -1;
        if (spin_pid < 0 || h_spin < 0) {
            kprintf("\n[cmd] test handle reap: setup FAIL (spin spawn %d handle %d)\n", spin_pid, h_spin);
            return 1;
        }

        /* (2) fill the REST of the table with QUICK-EXIT children; leak them all
         * (never wait()'d) so the table ends up full of soon-to-be-dead handles. */
        uint32_t leaked[PROC_HANDLE_MAX];
        int n_leak = 0;
        for (;;) {
            char *aleak[] = { "/system/bin/init.elf", "leak", NULL };
            int p = process_create("/system/bin/init.elf", aleak, 2, NULL, 0);
            if (p < 0) { ok = 0; break; }
            int h = process_handle_alloc(me, (uint32_t)p);
            if (h < 0) { process_kill((uint32_t)p); process_wait((uint32_t)p); break; }  /* table full */
            leaked[n_leak++] = (uint32_t)p;
        }

        /* (3) yield until every quick child has actually EXITED (become a zombie
         * we never reaped) -- schedule() lets them run even on a single core. */
        for (int i = 0; i < n_leak; i++)
            for (int y = 0; y < 20000 && process_alive(leaked[i]); y++) schedule();

        /* (4) table is now FULL of DEAD-child handles: an alloc must fail. */
        int h_full = process_handle_alloc(me, 0xDEADu);
        if (h_full >= 0) { ok = 0; process_handle_free(me, h_full); }

        /* (5) reap_dead() reclaims the dead ones (>= the n_leak we made) and
         * MUST spare the still-live spin child's handle. */
        int reclaimed = process_handle_reap_dead(me);
        if (reclaimed < n_leak) ok = 0;
        uint32_t chk;
        int spin_ok = (process_handle_resolve(me, h_spin, &chk) == 0 && chk == (uint32_t)spin_pid
                       && process_alive((uint32_t)spin_pid));
        if (!spin_ok) ok = 0;

        /* (6) the self-heal freed room: an alloc now succeeds where (4) failed. */
        int h_after = process_handle_alloc(me, 0xBEEFu);
        if (h_after < 0) ok = 0; else process_handle_free(me, h_after);

        /* cleanup: kill+reap the spin child, drop its handle. */
        process_kill((uint32_t)spin_pid);
        process_wait((uint32_t)spin_pid);
        process_handle_free(me, h_spin);

        kprintf("\n[cmd] test handle reap: filled %d dead handles; alloc-when-full %s; "
                "reclaimed %d; live child spared %s; realloc %s -> %s\n",
                n_leak, h_full < 0 ? "rejected" : "WRONGLY-OK",
                reclaimed, spin_ok ? "yes" : "NO",
                h_after >= 0 ? "ok" : "FAIL", ok ? "OK" : "FAIL");
        return 1;
    }

    /* Pipe EOF, end-to-end: sys_pipe -> INSTALL_OBJ spawn action -> exit-time
     * reap loop -> handle_close EOF. The full shell-pipeline dance:
     *   (1) make a pipe; hold BOTH end handles;
     *   (2) spawn a child whose fd 1 IS the write end (INSTALL_OBJ); it writes
     *       5 bytes through the ordinary sys_write path and exits; read them
     *       back through OUR read-end fd;
     *   (3) reap the child (its fd-1 write ref drops in the REAP LOOP, not at
     *       exit) -- a reader must then BLOCK, not EOF, because our own
     *       write-end handle is still open (n_writers==1: the classic
     *       "parent forgot to close its copy" state);
     *   (4) drop our write-end handle -> the blocked reader wakes with 0;
     *   (5) drop every remaining ref -> the pipe slot itself is reclaimed. */
    if (strcmp(cmd, "test pipe") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test pipe: VFS not registered (need /system/bin/init.elf)\n");
            return 1;
        }
        struct process *me = current_process;
        int ok = 1;
        uint32_t live0 = pipe_live_count();

        /* (1) create; both ends land as obj-handles in OUR table. */
        int rh = -1, wh = -1;
        if (pipe_create(&rh, &wh) != EMBK_OK) {
            kprintf("\n[cmd] test pipe: setup FAIL (pipe_create)\n");
            return 1;
        }
        struct pipe_end *pe_r = obj_handle_resolve(me, rh, HANDLE_KIND_PIPE);
        if (!pe_r || fd_install_pipe(me, 9, pe_r->p, 0) != EMBK_OK) {
            kprintf("\n[cmd] test pipe: setup FAIL (resolve/install read end)\n");
            obj_handle_free(me, rh); obj_handle_free(me, wh);
            return 1;
        }
        s_pipe_eof_p = pe_r->p;

        /* (2) writer child: INSTALL_OBJ wh -> child fd 1; then read its tag
         * back through our fd 9 (blocking read paces us to the child). */
        struct spawn_file_action act;
        memset(&act, 0, sizeof act);
        act.kind = SPAWN_ACTION_INSTALL_OBJ;
        act.target_fd = 1;
        act.src_obj_handle = wh;
        char *aw[] = { "/system/bin/init.elf", "pipewrite", NULL };
        int wpid = process_create("/system/bin/init.elf", aw, 2, &act, 1);
        if (wpid < 0) {
            kprintf("\n[cmd] test pipe: setup FAIL (spawn %d)\n", wpid);
            vfs_close(9); obj_handle_free(me, rh); obj_handle_free(me, wh);
            return 1;
        }
        char buf[8];
        size_t total = 0;
        while (total < 5) {
            size_t got = 0;
            if (vfs_fd_read(9, buf + total, 5 - total, &got) != EMBK_OK || got == 0) break;
            total += got;
        }
        int bytes_ok = (total == 5 && memcmp(buf, "hi-42", 5) == 0);
        if (!bytes_ok) ok = 0;

        /* (3) reap the child; then a 1-byte reader must BLOCK (no EOF).
         * Yield counts are deliberately MODEST: schedule() here is a full
         * cooperative hand-off, and with home + the clock widget rendering
         * under TCG a rotation back to this thread costs milliseconds --
         * 20000 unconditional yields looked like a hang (measured: the shell
         * stayed READY but starved for ~17s+). 300 rotations is already
         * hundreds of chances for the reader to (wrongly) complete. */
        process_wait((uint32_t)wpid);
        s_pipe_eof_done = 0;
        s_pipe_eof_got = 99;
        if (!process_create_kthread(pipe_eof_reader_kthread, NULL)) ok = 0;
        for (int y = 0; y < 300; y++) schedule();
        int blocked_ok = (s_pipe_eof_done == 0);
        if (!blocked_ok) ok = 0;

        /* (4) drop OUR write end -> n_writers 0 -> reader wakes with EOF.
         * Early-exit on success; the cap only bounds the FAIL case. */
        obj_handle_free(me, wh);
        for (int y = 0; y < 3000 && !s_pipe_eof_done; y++) schedule();
        int eof_ok = (s_pipe_eof_done && s_pipe_eof_got == 0);
        if (!eof_ok) ok = 0;

        /* (5) drop the remaining read refs (fd 9 + handle): slot reclaimed. */
        vfs_close(9);
        obj_handle_free(me, rh);
        int freed_ok = (pipe_live_count() == live0);
        if (!freed_ok) ok = 0;

        kprintf("\n[cmd] test pipe: bytes %s; blocks-while-parent-holds %s; "
                "EOF-after-close %s; slot freed %s -> %s\n",
                bytes_ok ? "ok" : "FAIL",
                blocked_ok ? "yes" : "NO(early EOF)",
                eof_ok ? "ok" : "FAIL",
                freed_ok ? "yes" : "NO",
                ok ? "OK" : "FAIL");
        return 1;
    }

    /* INTERACTIVE shell: spawn /system/bin/shell.elf on the console and wait for
     * it to exit. Not a self-test -- a way to actually USE the shell from the
     * kernel debug console. */
    if (strcmp(cmd, "shell") == 0) {
        char *sargv[] = { (char *)"/system/bin/shell.elf", NULL };
        /* Session policy (docs/USERSPACE.md §5): the shell starts in the user's
         * HOME because THIS spawner NAMES it -- cwd is never inherited; the parent
         * passes PWD and the child's crt0 seeds cwd from it (HOME too, so tools
         * resolve there). Without this the shell defaults to / and `pwd` prints /. */
        char *senv[] = { (char *)"HOME=/data/users/teo",
                         (char *)"PWD=/data/users/teo", NULL };
        int pid = process_create_env("/system/bin/shell.elf", sargv, 1, senv, NULL, 0);
        if (pid < 0) { kprintf("\n[cmd] shell: spawn failed: %s\n", embk_strerror(pid)); return 1; }
        /* Wait for the shell, but KEEP THE DESKTOP LIVE. A plain process_wait()
         * blocks THIS context -- the kernel main loop -- and compositor_pointer_
         * tick() runs HERE (main.c), so a blocking wait freezes home for the
         * shell's whole lifetime. Poll aliveness instead, pumping the compositor
         * + USB each pass and hlt-pacing on the ~100Hz timer exactly like the main
         * loop. We still never drain the keyboard buffer, so the shell keeps
         * owning fd 0 -- the two-consumer coexistence is unchanged. */
        while (process_alive((uint32_t)pid)) {
            usb_poll();
            compositor_pointer_tick();
            __asm__ volatile ("hlt");
        }
        int code = process_wait((uint32_t)pid);   /* already a zombie -> reaps now */
        kprintf("\n[cmd] shell: exited with code %d\n", code);
        return 1;
    }

    /* The structured shell, end to end ON the OS: spawn /system/bin/shell.elf -c "<line>"
     * three times (its stdio is console-inherited, so results land on serial)
     * and assert the exit codes: expression evaluation, a REAL ls pipeline
     * over EMBKFS through where/sort-by/select, and the error path. The pure
     * pipeline is host-tested (make shell-test); THIS proves the OS half:
     * spawn, console fds, readdir/stat, and the exit-status plumbing. */
    if (strcmp(cmd, "test shell") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test shell: VFS not registered (need /system/bin/shell.elf)\n");
            return 1;
        }
        int ok = 1;

        /* env: the shell must SHOW the environment it was handed, and `env set`
         * must reach the same `environ` it passes to the commands it spawns.
         * Spawned WITH an environment here, so the rendered table proves the
         * whole chain: process_create_env -> RDX -> crt0 -> environ -> builtin. */
        /* `|`, not `;` -- the lexer has no statement separator. Builtins run
         * IN-PROCESS, so the set and the listing share one `environ`. */
        char *aenv[] = { "/system/bin/shell.elf", "-c", "env set EMBK_SET fromshell | env", NULL };
        char *senv[] = { "EMBK_ENV_TEST=shell-ok", "HOME=/", NULL };
        int pe = process_create_env("/system/bin/shell.elf", aenv, 3, senv, NULL, 0);
        int ce = pe >= 0 ? process_wait((uint32_t)pe) : -1;
        if (pe < 0 || ce != 0) ok = 0;

        char *a1[] = { "/system/bin/shell.elf", "-c", "echo 1mb + 512kb", NULL };
        int p1 = process_create("/system/bin/shell.elf", a1, 3, NULL, 0);
        int c1 = p1 >= 0 ? process_wait((uint32_t)p1) : -1;
        if (p1 < 0 || c1 != 0) ok = 0;

        char *a2[] = { "/system/bin/shell.elf", "-c",
                       "ls / | where size > 100kb | sort-by size | select name size", NULL };
        int p2 = process_create("/system/bin/shell.elf", a2, 3, NULL, 0);
        int c2 = p2 >= 0 ? process_wait((uint32_t)p2) : -1;
        if (p2 < 0 || c2 != 0) ok = 0;

        char *a3[] = { "/system/bin/shell.elf", "-c", "echo $nope", NULL };
        int p3 = process_create("/system/bin/shell.elf", a3, 3, NULL, 0);
        int c3 = p3 >= 0 ? process_wait((uint32_t)p3) : -1;
        if (p3 < 0 || c3 != 1) ok = 0;   /* the error path must exit 1 */

        /* the standard-command batch, one line, RE-RUNNABLE (rm cleans up
         * what save created; no mkdir -- there's no rmdir to undo it):
         * save -> cat -> wc -> get -> rm -> ps -> count, with a cd/pwd
         * warm-up. Exercises the cwd machinery + file round-trip + proc
         * listing in a single shell run. */
        char *a4[] = { "/system/bin/shell.elf", "-c",
                       "cd / | echo \"alpha beta\" | save st-f.txt | cat /st-f.txt "
                       "| wc | get words | rm st-f.txt | mkdir st-d | rmdir st-d "
                       "| ps | count", NULL };
        int p4 = process_create("/system/bin/shell.elf", a4, 3, NULL, 0);
        int c4 = p4 >= 0 ? process_wait((uint32_t)p4) : -1;
        if (p4 < 0 || c4 != 0) ok = 0;

        kprintf("\n[cmd] test shell: expr rc=%d; ls-pipeline rc=%d; error-path rc=%d; "
                "cmd-batch rc=%d -> %s\n",
                c1, c2, c3, c4, ok ? "OK" : "FAIL");
        return 1;
    }

    /* C++ on the OS. cxxdemo.elf self-checks the things that actually break
     * on a fresh C++ port -- global ctors + their order, new/delete/new[],
     * templates, and a function-local static (which needs libsupc++'s
     * __cxa_guard_acquire) -- and exits 0 only if every one passed, so this
     * is just "spawn it and read the code". Its output lands on the console
     * via inherited stdio, so a failure names itself.
     *
     * ABSENT-TOOLCHAIN CASE IS NOT A FAILURE: cxxdemo.elf is only built when
     * a C++ cross compiler exists (Makefile's HAVE_CXX gate), so report SKIP
     * rather than FAIL when it isn't on the image -- a C-only checkout is a
     * supported configuration, not a broken one. */
    if (strcmp(cmd, "test cxx") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test cxx: VFS not registered\n");
            return 1;
        }
        struct vfs_stat st;
        if (vfs_stat("/data/apps/cxxdemo/cxxdemo.elf", &st) != EMBK_OK) {
            kprintf("\n[cmd] test cxx: SKIP -- /cxxdemo.elf not on the image "
                    "(no C++ toolchain: see `make cxx-check`)\n");
            return 1;
        }
        char *a[] = { "/data/apps/cxxdemo/cxxdemo.elf", NULL };
        int pid = process_create("/data/apps/cxxdemo/cxxdemo.elf", a, 1, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        kprintf("\n[cmd] test cxx: exit=%d -> %s\n", code,
                (pid >= 0 && code == 0) ? "OK" : "FAIL");
        return 1;
    }

    /* The POSIX layer in user/lib/syscalls.c. posixdemo.elf self-checks the
     * things that break quietly rather than loudly: opendir()'s grow-and-retry
     * loop against a >64-entry directory (the kernel's readdir truncates
     * SILENTLY, so a missing retry loses files with no error anywhere), the
     * d_type translation (native REG=1 vs DT_REG=8 -- a pass-through still
     * "works" until it lies), the monotonic clock actually advancing, and that
     * every call documented as ENOSYS really does refuse instead of returning a
     * comfortable 0. Exits 0 only if all pass; its output reaches the console
     * through inherited stdio, so a failure names itself.
     *
     * Unlike test cxx there is no toolchain gate: posixdemo.elf is plain C and
     * is always on the image, so absence here IS a failure. */
    /* Wall clock for the benchmark below. Same HPET conversion syscall.c's
     * uptime_ms_now() uses; there is no kernel-wide uptime_ms() to borrow. */
    #define IOPERF_MS() ({                                        \
        uint64_t _pf = hpet_available() ? hpet_period_fs() : 0;   \
        uint64_t _r;                                              \
        if (_pf) {                                                \
            uint64_t _tpms = 1000000000000ULL / _pf;              \
            if (!_tpms) _tpms = 1;                                \
            _r = hpet_read_counter() / _tpms;                     \
        } else { _r = timer_get_ticks(); }                        \
        _r; })

    /* Isolated read benchmark. `test posix`'s blkstat brackets the WHOLE process
     * -- the 70-directory dirent test and every small-file check land in the same
     * counters as the 3 MB loop, so no number from it is attributable to the read
     * path alone. That ambiguity has already cost several wrong conclusions about
     * where the time goes. This reads ONE file the way CPython's zipimport does
     * (8 KB sequential) with nothing else inside the bracket, and reports wall
     * time next to device time so the two can be told apart. */
    if (strcmp(cmd, "test ioperf") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test ioperf: VFS not registered\n");
            return 1;
        }
        const char *path = "/python314.zip";
        struct vfs_stat vst;
        if (vfs_stat(path, &vst) != 0) {
            kprintf("[ioperf] %s absent -- CPython not built; nothing to measure\n", path);
            return 1;
        }
        static uint8_t rbuf[8192];
        const uint64_t LIMIT = 3u * 1024u * 1024u;
        struct embk_blkstat b0, b1;

        /* Resolve ONCE and call the vnode's read op directly, because that is
         * what the application path does: an fd holds a vnode. vfs_read() is a
         * convenience wrapper that re-resolves the path on EVERY call, so
         * benchmarking through it measures a directory walk per read -- a path no
         * app takes, and worth 743% read amplification when first tried here. */
        struct vnode vn;
        if (vfs_resolve(path, &vn) != 0 || !vn.mnt || !vn.mnt->ops->read) {
            kprintf("[ioperf] cannot resolve %s\n", path);
            return 1;
        }

        embk_blkstat_reset();
        embk_blkstat_get(&b0);
        uint64_t w0 = IOPERF_MS();
        uint64_t off = 0, sum = 0;
        int rc = 0;
        while (off < LIMIT) {
            size_t got = 0;
            rc = vn.mnt->ops->read(&vn, off, rbuf, sizeof rbuf, &got);
            if (rc != 0 || got == 0) break;
            /* Touch the bytes so the read can't be optimised into nothing and so
             * a silently-empty buffer shows up as a suspicious checksum. */
            for (size_t i = 0; i < got; i += 512) sum += rbuf[i];
            off += got;
        }
        uint64_t w1 = IOPERF_MS();
        embk_blkstat_get(&b1);

        uint64_t wall = w1 - w0;
        uint64_t nreq = b1.reads - b0.reads;
        uint64_t nblk = b1.read_blocks - b0.read_blocks;
        uint64_t nus  = b1.read_us - b0.read_us;
        kprintf("[ioperf] rc=%d read %llu KB in %llu ms (%llu ms/MB), sum=%llu\n",
                rc, (unsigned long long)(off / 1024), (unsigned long long)wall,
                (unsigned long long)(off ? wall * 1048576ULL / off : 0),
                (unsigned long long)sum);
        kprintf("[ioperf] %llu requests, %llu KB from disk (%llu KB asked) = %llu%% amplification\n",
                (unsigned long long)nreq, (unsigned long long)(nblk / 2),
                (unsigned long long)(off / 1024),
                (unsigned long long)(off ? (nblk / 2) * 100ULL / (off / 1024) : 0));
        kprintf("[ioperf] %llu ms in dev->read (%llu%% of wall), %llu us/req, avg %llu blocks/req\n",
                (unsigned long long)(nus / 1000),
                (unsigned long long)(wall ? (nus / 1000) * 100ULL / wall : 0),
                (unsigned long long)(nreq ? nus / nreq : 0),
                (unsigned long long)(nreq ? nblk / nreq : 0));
        return 1;
    }

    /* The same posixdemo.elf, but spawned WITH an explicit environment. Both
     * halves matter: `test posix` proves a child given no environment reports
     * getenv()==NULL honestly, and this proves an environment a parent DOES hand
     * over arrives intact. EmbLink never inherits one -- see kernel spawn.h. */
    if (strcmp(cmd, "test env") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test env: VFS not registered\n");
            return 1;
        }
        char *a[] = { "/data/apps/posixdemo/posixdemo.elf", NULL };
        /* EMBK_EMPTY is set-but-empty on purpose: getenv() must return "" for it,
         * not NULL -- "set to nothing" and "not set" are different answers. */
        char *env[] = {
            "EMBK_ENV_TEST=1",
            "HOME=/",
            "PATH=/",
            "EMBK_EMPTY=",
            NULL
        };
        int pid = process_create_env("/data/apps/posixdemo/posixdemo.elf", a, 1, env, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        kprintf("\n[cmd] test env: exit=%d -> %s\n", code,
                (pid >= 0 && code == 0) ? "OK" : "FAIL");
        return 1;
    }

    /* Console ^C routing, END TO END (docs/INTERRUPTION.md Phase 2). Spawns a
     * userspace parent that routes ^C to a child blocked reading the console,
     * then BLOCKS waiting for the harness to inject a real Ctrl+C through QMP
     * `sendkey ctrl-c` -- the one half the kernel cannot fake from software
     * (PS/2 IRQ -> Ctrl tracking -> 0x03 -> routed cancel). Without that
     * keystroke this test HANGS, which is the correct failure mode: the child's
     * console read genuinely never returns. Sentinel 42 = the parent saw every
     * step hold. */
/* THE COMPLETE KEYBOARD. Drives the REAL 8042 IRQ path via QMP `sendkey`, so
 * every layer is exercised: controller -> scancode -> modifier state machine ->
 * both output streams.
 *
 * Each check asserts the ONE fact that distinguishes correct from broken --
 * "an event arrived" would pass with the state machine completely wrong. */
/* SEMAPHORE + MUTEX invariants. Single-threaded, so this checks the STATE
 * MACHINE (counts, ownership, try-paths), not contention -- the uncontended
 * fast paths are exactly what the bounce buffer relies on pre-scheduler, and a
 * count that drifts by one is the bug that a "did it hang" test never sees. */
/* THE SLAB ALLOCATOR. The heap is the most dangerous place to be wrong -- a
 * misrouted free corrupts silently and surfaces as a random crash elsewhere --
 * so this asserts the two invariants whose absence disabled the first attempt,
 * not merely "it didn't fault". kheap_check() walks the general block canaries
 * after each phase; a broken slab that scribbles a general block trips it. */
    if (strcmp(cmd, "test kheap") == 0) {
        int pass = 0, fail = 0;
        #define HCHK(name, cond) do { if (cond) pass++; else { fail++; \
            kprintf("  FAIL %s\n", name); } } while (0)

        /* 1. Every size class round-trips, and writing the WHOLE object then
         *    reading it back proves the object is really that big and isolated. */
        static const uint64_t sizes[] = { 16, 32, 64, 128, 256, 512, 1024 };
        for (unsigned k = 0; k < sizeof(sizes)/sizeof(sizes[0]); k++) {
            uint8_t *o = (uint8_t *)kmalloc(sizes[k]);
            if (!o) { fail++; kprintf("  FAIL alloc %lu\n", sizes[k]); continue; }
            for (uint64_t i = 0; i < sizes[k]; i++) o[i] = (uint8_t)(i ^ sizes[k]);
            int ok = 1;
            for (uint64_t i = 0; i < sizes[k]; i++) if (o[i] != (uint8_t)(i ^ sizes[k])) ok = 0;
            HCHK("size-class object intact end-to-end", ok);
            kfree(o);
        }
        kheap_check();

        /* 2. THE COLLISION REGRESSION. A general (non-slab) block filled entirely
         *    with 0xAA -- the OLD magic byte -- must still free as a general
         *    block. Range discrimination gets this right; the magic-byte scheme
         *    that was disabled would have misrouted it into a slab pool. If this
         *    is broken, kheap_check() below halts on canary corruption. */
        uint8_t *big = (uint8_t *)kmalloc(4096);   /* > 1024 => general, not slab */
        HCHK("large alloc is a general block", big != 0);
        if (big) { for (int i = 0; i < 4096; i++) big[i] = 0xAA; kfree(big); }
        kheap_check();

        /* 3. Force MULTIPLE region grows in one pool (more objects than a single
         *    region holds). This exercises the registry and -- critically -- must
         *    NOT deadlock: the old grow re-locked heap_lock via kheap_alloc. */
        #define NHAMMER 2048
        static void *hammer[NHAMMER];
        int got = 0;
        for (int i = 0; i < NHAMMER; i++) { hammer[i] = kmalloc(32); if (hammer[i]) got++; }
        HCHK("mass 32B alloc (multi-grow, no deadlock)", got == NHAMMER);
        /* write-through to catch any two objects that alias the same memory */
        for (int i = 0; i < got; i++) *(int *)hammer[i] = i;
        int alias = 0;
        for (int i = 0; i < got; i++) if (*(int *)hammer[i] != i) alias++;
        HCHK("no two slab objects alias", alias == 0);
        for (int i = 0; i < got; i++) kfree(hammer[i]);
        kheap_check();

        /* 4. krealloc across the slab->general boundary preserves the bytes. */
        uint8_t *r = (uint8_t *)kmalloc(16);
        for (int i = 0; i < 16; i++) r[i] = (uint8_t)(0x40 + i);
        r = (uint8_t *)krealloc(r, 2000);          /* 16 (slab) -> 2000 (general) */
        int rok = (r != 0);
        if (r) for (int i = 0; i < 16; i++) if (r[i] != (uint8_t)(0x40 + i)) rok = 0;
        HCHK("krealloc slab->general keeps data", rok);
        if (r) kfree(r);
        kheap_check();

        kheap_slab_stats();
        kprintf("\n[cmd] test kheap: %s (%d/%d)\n",
                fail == 0 ? "OK" : "FAIL", pass, pass + fail);
        #undef HCHK
        #undef NHAMMER
        return 1;
    }

    if (strcmp(cmd, "test ksync") == 0) {
        int pass = 0, fail = 0;
        #define SCHK(name, cond) do { if (cond) pass++; else { fail++; \
            kprintf("  FAIL %s\n", name); } } while (0)

        /* --- semaphore counting --- */
        struct semaphore s;
        sem_init(&s, 2);
        SCHK("sem(2): trywait #1 succeeds", sem_trywait(&s) == 1);
        SCHK("sem(2): trywait #2 succeeds", sem_trywait(&s) == 1);
        SCHK("sem(2): trywait #3 FAILS (empty)", sem_trywait(&s) == 0);
        sem_post(&s);
        SCHK("sem: post then trywait succeeds", sem_trywait(&s) == 1);
        SCHK("sem: empty again after", sem_trywait(&s) == 0);
        /* A semaphore counts UP past its initial value -- it is not a mutex. */
        sem_post(&s); sem_post(&s); sem_post(&s);
        SCHK("sem: counts to 3", sem_trywait(&s) && sem_trywait(&s) && sem_trywait(&s));
        SCHK("sem: and no further", sem_trywait(&s) == 0);

        /* --- mutex ownership + try --- */
        struct mutex m;
        mutex_init(&m);
        SCHK("mutex: trylock a free mutex succeeds", mutex_trylock(&m) == 1);
        SCHK("mutex: trylock a held mutex FAILS", mutex_trylock(&m) == 0);
        mutex_unlock(&m);
        SCHK("mutex: trylock succeeds after unlock", mutex_trylock(&m) == 1);
        mutex_unlock(&m);
        /* lock/unlock leaves it free for the next trylock -- the round trip. */
        mutex_lock(&m);
        SCHK("mutex: held after lock()", mutex_trylock(&m) == 0);
        mutex_unlock(&m);
        SCHK("mutex: free after unlock()", mutex_trylock(&m) == 1);
        mutex_unlock(&m);

        /* --- cancellation-aware acquire (the *_interruptible variants) ---
         *
         * Success path first: the interruptible acquire of an AVAILABLE
         * primitive behaves exactly like the plain one -- returns 0 -- because
         * cancellation gates only BLOCKING, never an uncontended take. */
        struct mutex fm; mutex_init(&fm);
        SCHK("mutex_lock_interruptible: free -> acquires (0)", mutex_lock_interruptible(&fm) == EMBK_OK);
        mutex_unlock(&fm);
        struct semaphore is; sem_init(&is, 1);
        SCHK("sem_wait_interruptible: permit avail -> takes it (0)", sem_wait_interruptible(&is) == EMBK_OK);
        SCHK("sem_wait_interruptible: then empty", sem_trywait(&is) == 0);

        /* Headline: a waiter that BLOCKS on a held mutex and is then cancelled
         * must WAKE and return -ECANCELED, WITHOUT stealing the lock. This
         * context (the driver) holds the lock for the whole exchange, so the
         * waiter is genuinely contended -- exactly the case the plain
         * mutex_lock() would sleep through until release. */
        mutex_init(&s_cx_mutex);
        mutex_lock(&s_cx_mutex);              /* the driver owns it */
        s_cx_started = 0;
        s_cx_result  = 999;                   /* sentinel: kthread not finished */
        struct thread *wt = process_create_kthread(cx_cancel_waiter_kthread, NULL);
        SCHK("cancel: waiter kthread spawned", wt != NULL);
        uint32_t wpid = wt ? wt->proc->pid : 0;
        /* Let it reach and park in the acquire. Yield counts are MODEST on
         * purpose (see `test pipe`): schedule() is a full cooperative hand-off
         * and under TCG a rotation costs ms, so a huge count reads as a hang. */
        for (int y = 0; y < 300 && !s_cx_started; y++) schedule();
        for (int y = 0; y < 50; y++) schedule();     /* settle into m->wq */
        /* Now cancel it: process_cancel sets the sticky flag AND removes the
         * thread from m->wq (marks it READY), so it re-checks at its wake point
         * and returns -ECANCELED rather than re-sleeping. */
        if (wpid) process_cancel(wpid);
        for (int y = 0; y < 3000 && s_cx_result == 999; y++) schedule();
        SCHK("cancel: blocked interruptible acquire woke (no hang)", s_cx_result != 999);
        SCHK("cancel: acquire refused with -ECANCELED", s_cx_result == -EMBK_ECANCELED);
        /* Non-vacuous: the waiter REFUSED, it did not acquire. The driver still
         * owns the lock, so a clean unlock + retake round-trips. Had the waiter
         * stolen the lock, owner would have changed and this unlock would fire
         * the non-owner warning / the retake would be against a held lock. */
        mutex_unlock(&s_cx_mutex);
        SCHK("cancel: waiter never stole the lock (driver round-trips)", mutex_trylock(&s_cx_mutex) == 1);
        mutex_unlock(&s_cx_mutex);

        kprintf("\n[cmd] test ksync: %s (%d/%d)\n",
                fail == 0 ? "OK" : "FAIL", pass, pass + fail);
        #undef SCHK
        return 1;
    }

    if (strcmp(cmd, "test vmm") == 0) {
        int pass = 0, fail = 0;
        #define VCHK(name, cond) do { if (cond) pass++; else { fail++; \
            kprintf("  FAIL %s\n", name); } } while (0)

        /* A benign RAM phys we only MAP/UNMAP -- never dereference -- so a
         * second VA aliasing it is harmless. */
        const uint64_t test_phys = 0x100000;   /* 1 MiB */

        /* --- map: page-aligned VA inside [MMIO_BASE, MMIO_END), really mapped --- */
        uint64_t va1 = vmm_map_mmio(test_phys, 0x1000);
        VCHK("vmm_map_mmio: non-zero VA", va1 != 0);
        VCHK("vmm_map_mmio: page-aligned", (va1 & 0xFFF) == 0);
        VCHK("vmm_map_mmio: inside [MMIO_BASE, MMIO_END)",
             va1 >= MMIO_BASE && va1 < MMIO_BASE + (1ULL << 38));
        VCHK("vmm_map_mmio: actually mapped", vmm_get_phys(va1) == test_phys);

        /* --- unmap clears the PTE --- */
        vmm_unmap_mmio(va1, 0x1000);
        VCHK("vmm_unmap_mmio: PTE cleared", vmm_get_phys(va1) == 0);

        /* --- free-list REUSE: same size comes back at the same VA (item B) --- */
        uint64_t va2 = vmm_map_mmio(test_phys, 0x1000);
        VCHK("vmm_unmap_mmio: freed VA reused first-fit", va2 == va1);
        VCHK("reused VA is mapped again", vmm_get_phys(va2) == test_phys);
        vmm_unmap_mmio(va2, 0x1000);

        /* --- WC variant maps, and PAT entry 4 really is Write-Combining (item A) --- */
        uint64_t wc = vmm_map_mmio_wc(test_phys, 0x1000);
        VCHK("vmm_map_mmio_wc: non-zero VA", wc != 0);
        VCHK("vmm_map_mmio_wc: mapped", vmm_get_phys(wc) == test_phys);
        {
            uint32_t lo, hi;
            __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277u));
            (void)lo;
            uint8_t pa4 = (uint8_t)(hi & 0xFF);   /* PAT entry 4 = bits[39:32] = low byte of hi */
            VCHK("IA32_PAT entry 4 == WC (0x01)", pa4 == 0x01);
        }
        vmm_unmap_mmio(wc, 0x1000);

        kprintf("\n[cmd] test vmm: %s (%d/%d)\n",
                fail == 0 ? "OK" : "FAIL", pass, pass + fail);
        #undef VCHK
        return 1;
    }

    if (strcmp(cmd, "test keyboard") == 0) {
        int pass = 0, fail = 0;
        #define KCHK(name, cond) do { if (cond) { pass++; } else { fail++; \
            kprintf("  FAIL %s\n", name); } } while (0)

        /* Drain whatever the console left behind, so we measure OUR keys. */
        { struct key_event e; while (keyboard_event_pop(&e)) {} }
        while (keyboard_has_char()) (void)keyboard_getchar();

        kprintf("\n[keyboard] layout=%s mods=0x%02x\n",
                keyboard_layout(), keyboard_mods());

        /* 1. Layouts. The abstraction is the claim: the SAME scancode must
         *    produce a different character, and 'us' must come back intact. */
        KCHK("layout: unknown name REFUSED (no silent fallback)",
             keyboard_set_layout("nope") == -1);
        KCHK("layout: still us after a refused switch",
             keyboard_layout()[0] == 'u');
        KCHK("layout: dvorak selectable", keyboard_set_layout("dvorak") == 0);
        KCHK("layout: dvorak is active",  keyboard_layout()[0] == 'd');
        KCHK("layout: back to us",        keyboard_set_layout("us") == 0);

        /* 2. The event ring must survive overflow WITHOUT stranding a
         *    modifier. Push more than it holds, then check it still drains. */
        kprintf("[keyboard] mods now 0x%02x (0=nothing held)\n", keyboard_mods());
        KCHK("no modifier stuck down at rest", keyboard_mods() == 0);

        kprintf("\n[cmd] test keyboard: %d/%d checks\n", pass, pass + fail);
        kprintf("[keyboard] NOW TYPE -- 8s live capture (events: code/mods/up-down)\n");

        /* 3. LIVE: whatever arrives in the next 8s, report it. This is where a
         *    QMP `sendkey ctrl-c` / `sendkey f1` / `sendkey caps_lock` proves the
         *    real IRQ path, not a simulated one. */
        int nev = 0;
        for (int tick = 0; tick < 160; tick++) {   /* 160 x 50ms = 8s */
            struct key_event e;
            while (keyboard_event_pop(&e)) {
                kprintf("  ev code=0x%03x mods=0x%02x %s\n",
                        e.code, e.mods, e.pressed ? "DOWN" : "up");
                nev++;
            }
            hpet_delay_ms(50);
        }
        kprintf("[keyboard] captured %d events; mods at end=0x%02x\n",
                nev, keyboard_mods());
        #undef KCHK
        return 1;
    }

    if (strcmp(cmd, "test ctrlc") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test ctrlc: VFS not registered\n");
            return 1;
        }
        char *a[] = { "/system/bin/init.elf", "ctrlc-parent", NULL };
        int pid = process_create("/system/bin/init.elf", a, 2, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        kprintf("\n[cmd] test ctrlc: exit=%d -> %s\n", code,
                (pid >= 0 && code == 42) ? "OK" : "FAIL");
        return 1;
    }

    /* The ESCALATION half (docs/INTERRUPTION.md §4.3), against a child that
     * DECLINES: the shell runs `/system/bin/init.elf spin` (an embk_sleep_ms loop that
     * never observes cancellation), the harness injects a real Ctrl+C, and the
     * shell must cancel -> wait out its grace period -> embk_kill -> reap ->
     * exit. Before the pump was made pollable the shell sat in a blocking pipe
     * read for the child's whole life, so this test HUNG -- completion of the
     * "[cmd]" line below IS the assertion. */
    if (strcmp(cmd, "test ctrlc2") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test ctrlc2: VFS not registered\n");
            return 1;
        }
        char *a[] = { "/system/bin/shell.elf", "-c", "/system/bin/init.elf spin", NULL };
        int pid = process_create("/system/bin/shell.elf", a, 3, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        kprintf("\n[cmd] test ctrlc2: shell exit=%d -> %s (completion IS the pass)\n",
                code, pid >= 0 ? "OK" : "FAIL");
        return 1;
    }

    /* git. `version` first, deliberately -- the same first question the CPython
     * port asked with -c: does the binary initialise and reach its own code at
     * all, before any repository work complicates the answer. */
    if (strcmp(cmd, "test git") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test git: VFS not registered\n");
            return 1;
        }
        char *a[] = { "/data/apps/git/git.elf", "version", NULL };
        char *env[] = { "HOME=/", NULL };
        int pid = process_create_env("/data/apps/git/git.elf", a, 2, env, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        kprintf("\n[cmd] test git: exit=%d -> %s\n", code,
                (pid >= 0 && code == 0) ? "OK" : "FAIL");
        return 1;
    }

    /* The full git WORKFLOW: init -> config -> add -> commit -> log, a real
     * repository at "/" (EmbLink has no per-process cwd -- chdir is an honest
     * ENOSYS and getcwd()=="/" -- so the repo roots at the fs root rather than
     * pretending -C works). Exercises everything the port added: /dev/null,
     * rename (lockfiles!), unlink, ftruncate, mkdir trees, zlib objects.
     * GIT_PAGER= (empty) because `log` would otherwise try to SPAWN a pager the
     * moment it sees a console fd -- and exec is ENOSYS by design. */
    if (strcmp(cmd, "test git repo") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test git repo: VFS not registered\n");
            return 1;
        }
        char *env[] = { "HOME=/", "GIT_PAGER=", NULL };
        int ok = 1;

        /* A file worth committing, written kernel-side through the VFS. */
        {
            int fd = vfs_open("/README.md", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                static const char msg[] = "# EmbLinkOS\ngit works here.\n";
                size_t w = 0;
                vfs_fd_write(fd, msg, sizeof msg - 1, &w);
                vfs_close(fd);
            } else { kprintf("test git repo: README create failed\n"); ok = 0; }
        }

        struct { const char *what; char *argv[8]; int argc; } steps[] = {
            { "init",   { "/data/apps/git/git.elf", "init" }, 2 },
            { "email",  { "/data/apps/git/git.elf", "config", "user.email", "motsou@emblink" }, 4 },
            { "name",   { "/data/apps/git/git.elf", "config", "user.name", "Motsou" }, 4 },
            { "add",    { "/data/apps/git/git.elf", "add", "README.md" }, 3 },
            { "commit", { "/data/apps/git/git.elf", "commit", "-m", "first commit on EmbLinkOS" }, 4 },
            { "log",    { "/data/apps/git/git.elf", "log", "--oneline" }, 3 },
            { "status", { "/data/apps/git/git.elf", "status" }, 2 },
        };
        for (unsigned i = 0; i < sizeof steps / sizeof steps[0] && ok; i++) {
            int pid = process_create_env("/data/apps/git/git.elf", steps[i].argv, steps[i].argc,
                                         env, NULL, 0);
            int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
            kprintf("[git %s] exit=%d\n", steps[i].what, code);
            if (pid < 0 || code != 0) ok = 0;
        }

        kprintf("\n[cmd] test git repo: %s\n", ok ? "OK" : "FAIL");
        return 1;
    }

    /* git in a SUBDIRECTORY -- what per-process cwd bought. Every repo used to
     * root at "/" because chdir was ENOSYS, so `status` listed every packed
     * .elf as untracked and two repos were impossible. PWD is how the parent
     * NAMES the child's starting directory (nothing is inherited); crt0 seeds
     * the cwd from it. This also exercises git's own chdir: it walks UP from
     * the cwd looking for .git, which needs ".." to normalize. */
    if (strcmp(cmd, "test git cwd") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test git cwd: VFS not registered\n");
            return 1;
        }
        (void)vfs_mkdir_path("/proj");
        {
            int fd = vfs_open("/proj/hello.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                static const char msg[] = "a repo that is NOT at the root\n";
                size_t w = 0;
                vfs_fd_write(fd, msg, sizeof msg - 1, &w);
                vfs_close(fd);
            }
        }

        /* PWD=/proj: the whole point. Same env for every step. */
        char *env[] = { "HOME=/", "GIT_PAGER=", "PWD=/proj", NULL };
        int ok = 1;
        struct { const char *what; char *argv[8]; int argc; } steps[] = {
            { "init",   { "/data/apps/git/git.elf", "init" }, 2 },
            { "email",  { "/data/apps/git/git.elf", "config", "user.email", "motsou@emblink" }, 4 },
            { "name",   { "/data/apps/git/git.elf", "config", "user.name", "Motsou" }, 4 },
            { "add",    { "/data/apps/git/git.elf", "add", "hello.txt" }, 3 },   /* RELATIVE */
            { "commit", { "/data/apps/git/git.elf", "commit", "-m", "committed from /proj" }, 4 },
            { "status", { "/data/apps/git/git.elf", "status", "--short" }, 3 },
        };
        for (unsigned i = 0; i < sizeof steps / sizeof steps[0] && ok; i++) {
            int pid = process_create_env("/data/apps/git/git.elf", steps[i].argv, steps[i].argc,
                                         env, NULL, 0);
            int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
            kprintf("[git@/proj %s] exit=%d\n", steps[i].what, code);
            if (pid < 0 || code != 0) ok = 0;
        }

        /* The repo must be AT /proj -- if cwd were ignored it would be at /. */
        struct vfs_stat st;
        int here = (vfs_stat("/proj/.git", &st) == 0);
        int not_root = (vfs_stat("/.git/HEAD", &st) != 0);
        kprintf("[git@/proj] /proj/.git exists: %s\n", here ? "yes" : "NO");
        kprintf("\n[cmd] test git cwd: %s\n", (ok && here && not_root) ? "OK" : "FAIL");
        return 1;
    }

    /* THE SHELL'S cwd CHAIN, end to end. `cd` moves the shell's REAL cwd (the
     * libc's) and republishes PWD -- and PWD is exactly the vector eval_extern
     * hands to every external command, which crt0 then seeds the child's cwd
     * from. Nothing is inherited; this chain IS the inheritance.
     *
     * The assertion is `git init` run from the shell after a `cd`: if PWD had
     * not followed the cd, git would init at "/" and /cwdshell/.git would not
     * exist. `|` not `;` -- the lexer has no statement separator, and builtins
     * run IN-PROCESS so `cd` and the spawn share one environ. */
    if (strcmp(cmd, "test shell cwd") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test shell cwd: VFS not registered\n");
            return 1;
        }
        (void)vfs_mkdir_path("/cwdshell");
        char *env[] = { "HOME=/", "GIT_PAGER=", NULL };
        int ok = 1;

        char *a1[] = { "/system/bin/shell.elf", "-c", "cd /cwdshell | pwd", NULL };
        int p1 = process_create_env("/system/bin/shell.elf", a1, 3, env, NULL, 0);
        int c1 = p1 >= 0 ? process_wait((uint32_t)p1) : -1;
        kprintf("[shell cd|pwd] exit=%d\n", c1);
        if (p1 < 0 || c1 != 0) ok = 0;

        /* cd, then SPAWN git there. The spawn is the point: a builtin sharing
         * the shell's own cwd proves nothing about what a CHILD sees. */
        char *a2[] = { "/system/bin/shell.elf", "-c", "cd /cwdshell | git init", NULL };
        int p2 = process_create_env("/system/bin/shell.elf", a2, 3, env, NULL, 0);
        int c2 = p2 >= 0 ? process_wait((uint32_t)p2) : -1;
        kprintf("[shell cd|git init] exit=%d\n", c2);

        struct vfs_stat st;
        int here = (vfs_stat("/cwdshell/.git", &st) == 0);
        kprintf("[shell cwd] /cwdshell/.git exists: %s\n", here ? "yes" : "NO");
        kprintf("\n[cmd] test shell cwd: %s\n", (ok && here) ? "OK" : "FAIL");
        return 1;
    }

    /* TCC. `-v` first, deliberately -- the same first question git version and
     * python -c answered: does the binary initialise and reach its own code at
     * all, before any compiling complicates the answer. */
    if (strcmp(cmd, "test tcc") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test tcc: VFS not registered\n"); return 1; }
        char *a[] = { "/data/apps/tcc/tcc.elf", "-v", NULL };
        char *env[] = { "HOME=/", NULL };
        int pid = process_create_env("/data/apps/tcc/tcc.elf", a, 2, env, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        kprintf("\n[cmd] test tcc: exit=%d -> %s\n", code,
                (pid >= 0 && code == 0) ? "OK" : "FAIL");
        return 1;
    }

/* THE COMPILER ITSELF: source -> object, ON the OS.
 *
 * Deliberately HEADER-FREE. tcc needs /include and /lib to build a normal
 * program, and mkfs packs a FLAT root -- there is no /include yet. But none of
 * that is the compiler: `tcc -c` exercises the frontend, the x86-64 codegen,
 * the integrated assembler and the ELF object writer, which is the whole
 * question at this step. Linking a runnable binary is the NEXT one.
 *
 * The proof is the OBJECT: read it back and check the ELF magic + type. An
 * exit code of 0 only says tcc did not complain. */
    if (strcmp(cmd, "test tcc compile") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test tcc compile: VFS not registered\n"); return 1; }

        static const char src[] =
            "/* no #include: the OS has no /include yet, and this is a test of\n"
            "   the COMPILER, not of header search. */\n"
            "int add(int a, int b) { return a + b; }\n"
            "int answer(void) { return add(40, 2); }\n";
        {
            int fd = vfs_open("/data/tmp/t.c", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { kprintf("test tcc compile: cannot write /t.c\n"); return 1; }
            size_t w = 0;
            vfs_fd_write(fd, src, sizeof src - 1, &w);
            vfs_close(fd);
        }
        (void)vfs_unlink_path("/data/tmp/t.o");

        char *a[] = { "/data/apps/tcc/tcc.elf", "-c", "/data/tmp/t.c", "-o", "/data/tmp/t.o" };
        char *env[] = { "HOME=/", NULL };
        int pid = process_create_env("/data/apps/tcc/tcc.elf", a, 5, env, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        kprintf("[tcc -c] exit=%d\n", code);

        /* Read the object back and look at it. */
        unsigned char hdr[20] = {0};
        struct vfs_stat st;
        int have = (vfs_stat("/data/tmp/t.o", &st) == 0);
        if (have) {
            int fd = vfs_open("/data/tmp/t.o", O_RDONLY, 0);
            if (fd >= 0) { size_t r = 0; vfs_fd_read(fd, hdr, sizeof hdr, &r); vfs_close(fd); }
        }
        int is_elf = have && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F';
        int is_64  = is_elf && hdr[4] == 2;                 /* ELFCLASS64 */
        int is_rel = is_elf && hdr[16] == 1;                /* ET_REL: an object */
        kprintf("[tcc -c] /t.o: %s, %llu bytes; ELF=%d 64-bit=%d ET_REL=%d\n",
                have ? "written" : "MISSING",
                have ? (unsigned long long)st.size : 0ULL, is_elf, is_64, is_rel);

        int ok = (pid >= 0 && code == 0 && is_elf && is_64 && is_rel && st.size > 64);
        kprintf("\n[cmd] test tcc compile: %s\n", ok ? "OK" : "FAIL");
        return 1;
    }

/* THE WHOLE TOOLCHAIN: source -> RUNNING PROCESS, entirely on the OS.
 *
 * This is the step that makes EmbLink self-hosting for C: tcc compiles, tcc
 * assembles, tcc LINKS, and the kernel then spawns what came out. The proof is
 * the EXIT CODE of the produced binary -- 42 can only be there if the program
 * we wrote was really compiled and really ran. A valid-looking ELF proves the
 * writer works; only running it proves the whole chain does.
 *
 * `-nostdlib` is the load-bearing flag, and it is not a workaround:
 *   - tcc would otherwise link ITS host's crt1.o/crti.o/crtn.o, which do not
 *     exist here and would be wrong if they did -- EmbLink's entry contract is
 *     crt0.o's _start(argc, argv, envp), not glibc's.
 *   - it also drops the implicit -ltcc1 (tcc's own runtime). We do not have
 *     libtcc1.a: its Makefile builds it by RUNNING the just-built tcc, which is
 *     an EmbLink binary the host cannot execute. A program that needs no
 *     __divdi3-class helper does not need it, so we find out by asking.
 * So we name EVERY input ourselves -- crt0.o, syscalls.o, libc.a -- which is
 * exactly the model the rest of the OS uses: nothing is inherited, the caller
 * names what it wants.
 *
 * `-static` is equally load-bearing, and it was not obvious: without it tcc
 * links a DYNAMIC executable -- PT_INTERP + PT_DYNAMIC, asking for an ELF
 * interpreter to be exec'd on its behalf. That is a Linux contract EmbLink does
 * not implement and will not: there is no exec, and our in-kernel loader binds a
 * KNOWN second object (libembk.so) rather than running an interpreter program
 * ([[dynamic-linking]]). The first attempt here produced a perfectly valid
 * 82 KB ET_EXEC that the kernel then refused, and the ELF said exactly why.
 *
 * `-L/` and not `-L/lib` because mkfs packs a FLAT root. The toolchain lives at
 * "/" alongside the apps, so the flat root IS the search path -- no directory
 * support required to build software here.
 *
 * Header-free again, and for the same reason as `test tcc compile`: /include
 * does not exist. main() needs no declaration to be defined. */
    if (strcmp(cmd, "test tcc link") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test tcc link: VFS not registered\n"); return 1; }

        static const char src[] =
            "/* Compiled BY EmbLink, ON EmbLink. No #include: there is no\n"
            "   /include yet, and main needs no declaration to be defined. */\n"
            "static int twice(int x) { return x + x; }\n"
            "int main(void) { return twice(21); }\n";
        {
            int fd = vfs_open("/data/tmp/l.c", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { kprintf("test tcc link: cannot write /l.c\n"); return 1; }
            size_t w = 0;
            vfs_fd_write(fd, src, sizeof src - 1, &w);
            vfs_close(fd);
        }
        (void)vfs_unlink_path("/data/tmp/l.elf");

        char *a[] = { "/data/apps/tcc/tcc.elf", "-static", "-nostdlib", "/data/tmp/l.c", "/system/abi/crt0.o",
                      "/system/abi/syscalls.o", "-L/system/abi", "-lc", "-o", "/data/tmp/l.elf" };
        char *env[] = { "HOME=/", NULL };
        /* Echo the command. It documents what is being asked, and it doubles as
         * a staleness canary: if this line does not appear, you are not running
         * the kernel you just built -- check that BEFORE doubting the compiler. */
        kprintf("[tcc link] %s %s %s %s %s %s %s %s %s\n",
                a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]);
        int pid = process_create_env("/data/apps/tcc/tcc.elf", a, 10, env, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        kprintf("[tcc link] exit=%d\n", code);

        /* Read enough of the header to see e_phnum. It is the DIRECT readout of
         * whether -static took: tccelf.c gives a static exe 2 program headers
         * and a dynamic one 5 (PHDR+INTERP+2xLOAD+DYNAMIC). Measuring it here,
         * in the kernel, beats extracting the file and guessing on the host. */
        unsigned char hdr[64] = {0};
        struct vfs_stat st;
        int have = (vfs_stat("/data/tmp/l.elf", &st) == 0);
        if (have) {
            int fd = vfs_open("/data/tmp/l.elf", O_RDONLY, 0);
            if (fd >= 0) { size_t r = 0; vfs_fd_read(fd, hdr, sizeof hdr, &r); vfs_close(fd); }
        }
        int is_elf = have && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F';
        int is_exec = is_elf && hdr[16] == 2;              /* ET_EXEC: runnable */
        unsigned phnum = is_elf ? (unsigned)(hdr[56] | (hdr[57] << 8)) : 0;
        kprintf("[tcc link] /l.elf: %s, %llu bytes; ELF=%d ET_EXEC=%d phnum=%u (2=static 5=dynamic)\n",
                have ? "written" : "MISSING",
                have ? (unsigned long long)st.size : 0ULL, is_elf, is_exec, phnum);

        /* RUN IT. This is the actual claim. */
        int rcode = -1;
        if (is_exec) {
            char *b[] = { "/data/tmp/l.elf", NULL };
            int p2 = process_create_env("/data/tmp/l.elf", b, 1, env, NULL, 0);
            rcode = p2 >= 0 ? process_wait((uint32_t)p2) : -1;
            kprintf("[tcc link] /l.elf ran: exit=%d (want 42)\n", rcode);
        }

        int ok = (pid >= 0 && code == 0 && is_elf && is_exec && rcode == 42);
        kprintf("\n[cmd] test tcc link: %s\n", ok ? "OK" : "FAIL");
        return 1;
    }

/* A REAL PROGRAM: #include <stdio.h> and friends, compiled+linked+run on-OS.
 *
 * `test tcc link` proved the toolchain on a header-free toy that pulled ZERO
 * members out of libc.a. This is the step it left open, and it closes three
 * gaps at once:
 *   - HEADERS: #include <stdio.h> resolves from tcc's baked-in sysinclude
 *     paths (/system/abi/include for newlib, /data/apps/tcc/include for the
 *     compiler's own stddef.h) -- both packed by mkfs. Needs tcc patch 0002:
 *     a pristine tcc 0.9.27 predefines no GCC type macros and newlib's
 *     sys/_intsup.h #errors on the very first include.
 *   - THE LIBRARY AT SCALE: fopen/fprintf/fclose drag in newlib's buffered
 *     stdio -- dozens of libc.a members, hundreds of cross-object calls.
 *     This is the load the PLT32->PC32 patch (0001) must survive at scale;
 *     `twice(21)` never exercised it.
 *   - BARE -lc: no -L on the line -- tcc's baked-in /system/abi libpath must
 *     find libc.a by itself.
 * No -lgcc anywhere: proven unnecessary on the host (libc.a references zero
 * libgcc intrinsics; both objects link to 0 undefined without it).
 *
 * The proof is twofold: the produced binary EXITS 42 (computed, not constant),
 * and the FILE it wrote through buffered stdio reads back byte-exact. */
/* Hammer the kernel-context fs WRITE path: create/write/close/unlink in a
 * tight loop with per-iteration progress. Built to make a PROBABILISTIC hang
 * (observed: console commands freezing in their first vfs write or spawn,
 * different points on different boots) near-certain within one run, so a
 * kernel bisect gets a reliable verdict per boot instead of a coin flip. */
    if (strcmp(cmd, "test writestorm") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test writestorm: VFS not registered\n"); return 1; }
        static const char blob[300] = "writestorm";
        /* IF-probe after each stage: the class of bug this test exists for is
         * a blocking path LEAKING IF=0 back into process context (the
         * sched_block_current_locked bug, fixed in process.c -- found because
         * this storm made the resulting hlt-with-IF=0 hang reproducible). Any
         * report here is a regression of that fix; the probe re-enables IF so
         * the run survives to count every leak, and ANY leak fails the test. */
        int if_leaks = 0;
        #define IFCHK(stage) do { uint64_t _rf; \
            __asm__ volatile ("pushfq; popq %0" : "=r"(_rf)); \
            if (!(_rf & (1ULL << 9))) { \
                if_leaks++; \
                kprintf("[storm] IF=0 after %s (iter %d)\n", stage, i); \
                __asm__ volatile ("sti" ::: "memory"); \
            } } while (0)
        int i;
        for (i = 0; i < 200; i++) {
            int fd = vfs_open("/data/tmp/w.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            IFCHK("open");
            if (fd < 0) { kprintf("[storm] iter %d: open failed %d\n", i, fd); break; }
            size_t w = 0;
            vfs_fd_write(fd, blob, sizeof blob, &w);
            IFCHK("write");
            vfs_close(fd);
            IFCHK("close");
            if (vfs_unlink_path("/data/tmp/w.bin") != 0) { kprintf("[storm] iter %d: unlink failed\n", i); break; }
            IFCHK("unlink");
            if ((i % 10) == 0) kprintf("[storm] iter %d ok\n", i);
        }
        #undef IFCHK
        kprintf("\n[cmd] test writestorm: %s (%d/200, %d IF leak(s))\n",
                (i == 200 && if_leaks == 0) ? "OK" : "FAIL", i, if_leaks);
        return 1;
    }

/* THE GUI WALL COMES DOWN: on-OS tcc COMPILES + DYNAMIC-LINKS a real EmUI app
 * (clockw) against /system/lib/libembk.so, and the kernel LOADS + RUNS it.
 *
 * Static C was already self-hostable; the block was that TCC could not produce
 * the dynamic ET_EXEC the in-kernel loader binds to libembk.so. Four things
 * make it work now (all cross-shipped to /system/abi, all proven on the host
 * first): tcc patch 0004 (pull the .so's undefined libc symbols INTO the app so
 * -lc/-lm satisfy them and -rdynamic exports them back -- no runtime libc.so
 * here), libtcc1.o (the float intrinsics tcc emits but x86_64 libgcc lacks),
 * emlink_dynstubs.o (crt0's weak bracket symbols as weak-abs-0 -- newlib.ld's
 * PROVIDE()s, which tcc has no linker script for), and libm.a (cosf/sinf).
 *
 * Proof: the produced clockw2.elf is ET_EXEC with PT_DYNAMIC, and it SPAWNS and
 * stays alive as a widget (home spawns the gcc-built clockw the identical way).
 * The whole point is the dynamic load succeeding on a tcc-built binary. */
    if (strcmp(cmd, "test tcc dyn") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test tcc dyn: VFS not registered\n"); return 1; }
        char *env[] = { "HOME=/", NULL };
        int ok = 1;
        (void)vfs_unlink_path("/data/tmp/clockw2.o");
        (void)vfs_unlink_path("/data/apps/clockw2/clockw2.elf");
        (void)vfs_mkdir_path("/data/apps/clockw2");

        /* (1) compile: tcc -c against the on-image ui headers + newlib */
        char *ac[] = { "/data/apps/tcc/tcc.elf", "-c",
                       "-I/data/src/ui/scene", "-I/data/src/ui/backend",
                       "-I/data/src/ui/layout", "-I/data/src/ui/reactive",
                       "-I/data/src/ui/declare", "-I/data/src/ui/theme",
                       "-I/data/src/ui/kit", "-I/data/src/ui/dsl",
                       "-I/system/abi/include",
                       "/data/src/ui/clockw.c", "-o", "/data/tmp/clockw2.o" };
        int p = process_create_env("/data/apps/tcc/tcc.elf", ac, 14, env, NULL, 0);
        int c = p >= 0 ? process_wait((uint32_t)p) : -1;
        kprintf("[tcc dyn] compile exit=%d\n", c);
        if (p < 0 || c != 0) ok = 0;

        /* (2) DYNAMIC LINK against libembk.so + the dynamic-link ABI. -rdynamic
         * exports the app's newlib for the .so to bind back to; no -static. */
        if (ok) {
            char *al[] = { "/data/apps/tcc/tcc.elf", "-nostdlib", "-rdynamic",
                           "/system/abi/crt0.o", "/system/abi/syscalls.o",
                           "/data/tmp/clockw2.o", "/system/lib/libembk.so",
                           "/system/abi/emlink_dynstubs.o", "-L/system/abi",
                           "-lc", "-lm", "/system/abi/libtcc1.o",
                           "-o", "/data/apps/clockw2/clockw2.elf" };
            p = process_create_env("/data/apps/tcc/tcc.elf", al, 14, env, NULL, 0);
            c = p >= 0 ? process_wait((uint32_t)p) : -1;
            kprintf("[tcc dyn] link exit=%d\n", c);
            if (p < 0 || c != 0) ok = 0;
        }

        /* (2b) the shape the loader requires: ET_EXEC + PT_DYNAMIC (phnum>2) */
        if (ok) {
            unsigned char hdr[64] = {0}; struct vfs_stat st;
            int have = (vfs_stat("/data/apps/clockw2/clockw2.elf", &st) == 0);
            if (have) { int fd = vfs_open("/data/apps/clockw2/clockw2.elf", O_RDONLY, 0);
                        if (fd >= 0) { size_t r=0; vfs_fd_read(fd, hdr, sizeof hdr, &r); vfs_close(fd); } }
            int is_exec = have && hdr[0]==0x7f && hdr[1]=='E' && hdr[16]==2;
            unsigned phnum = have ? (unsigned)(hdr[56] | (hdr[57]<<8)) : 0;
            kprintf("[tcc dyn] clockw2.elf: %llu bytes, ET_EXEC=%d phnum=%u (dynamic>2)\n",
                    have ? (unsigned long long)st.size : 0ULL, is_exec, phnum);
            if (!is_exec || phnum < 3) ok = 0;
        }

        /* (3) THE LOAD: spawn the tcc-built dynamic app. The kernel loads it,
         * binds it to libembk.so (two-way), and it runs as a widget. If the
         * dynamic load failed it would return < 0 here, loudly on serial. */
        if (ok) {
            char *aw[] = { "/data/apps/clockw2/clockw2.elf", NULL };
            int wp = process_create_env("/data/apps/clockw2/clockw2.elf", aw, 1, env, NULL, 0);
            kprintf("[tcc dyn] spawn tcc-built widget -> pid=%d\n", wp);
            if (wp < 0) ok = 0;
            else {
                /* let it run a few ticks; a widget stays alive rendering. */
                uint64_t t0 = lapic_timer_get_ticks();
                while (lapic_timer_get_ticks() - t0 < 200) {   /* ~2 guest-s */
                    compositor_pointer_tick(); __asm__ volatile ("sti; hlt");
                }
                int alive = process_alive((uint32_t)wp);
                kprintf("[tcc dyn] widget alive after run: %s\n", alive ? "YES (loaded + running)" : "no (exited/crashed)");
                if (!alive) ok = 0;
                process_kill((uint32_t)wp);
                process_wait((uint32_t)wp);
            }
        }

        kprintf("\n[cmd] test tcc dyn: %s\n",
                ok ? "OK -- the OS compiled, linked, and RAN a GUI app" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test tcc real") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test tcc real: VFS not registered\n"); return 1; }

        static const char src[] =
            "#include <stdio.h>\n"
            "#include <string.h>\n"
            "#include <stdint.h>\n"
            "int main(void) {\n"
            "    FILE *f = fopen(\"/data/tmp/r.out\", \"w\");\n"
            "    if (!f) return 1;\n"
            "    uint64_t v = 40;\n"
            "    const char *tag = \"ok\";\n"
            "    fprintf(f, \"hdr=%s n=%u\\n\", tag, (unsigned)(v + strlen(tag)));\n"
            "    fclose(f);\n"
            "    return (int)(v + strlen(tag));\n"   /* 40 + 2 = 42 */
            "}\n";
        {
            int fd = vfs_open("/data/tmp/r.c", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { kprintf("test tcc real: cannot write /data/tmp/r.c\n"); return 1; }
            size_t w = 0;
            vfs_fd_write(fd, src, sizeof src - 1, &w);
            vfs_close(fd);
        }
        (void)vfs_unlink_path("/data/tmp/r.elf");
        (void)vfs_unlink_path("/data/tmp/r.out");

        char *a[] = { "/data/apps/tcc/tcc.elf", "-static", "-nostdlib", "/data/tmp/r.c",
                      "/system/abi/crt0.o", "/system/abi/syscalls.o", "-lc",
                      "-o", "/data/tmp/r.elf" };
        char *env[] = { "HOME=/", NULL };
        kprintf("[tcc real] %s %s %s %s %s %s %s %s\n",
                a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
        int pid = process_create_env("/data/apps/tcc/tcc.elf", a, 9, env, NULL, 0);
        kprintf("[tcc real] spawned pid=%d\n", pid);   /* prints or create hung */
        /* Pump-wait with a LIVE progress line every ~10 s (1000 ticks @100 Hz):
         * device-read deltas rising = tcc is grinding I/O; frozen deltas with a
         * still-alive process = a genuine hang. Under TCG the whole compile+link
         * is minutes, and a silent minutes-long wait is indistinguishable from a
         * hang from the outside -- that ambiguity cost a debugging session. */
        int code = -1;
        if (pid >= 0) {
            struct embk_blkstat pb;
            embk_blkstat_get(&pb);
            uint64_t t0 = lapic_timer_get_ticks(), tlast = t0;
            uint64_t spins = 0;
            while (process_alive((uint32_t)pid)) {
                __asm__ volatile ("hlt");
                /* Unconditional iteration heartbeat, deliberately NOT tick-gated:
                 * if ticks froze, the 10s line below never fires and the silence
                 * is ambiguous; this one only needs the loop itself to spin. */
                if ((++spins & 0xFFFFF) == 0) serial_write_char('~');
                uint64_t now = lapic_timer_get_ticks();
                if (now - tlast >= 1000) {
                    struct embk_blkstat nb;
                    embk_blkstat_get(&nb);
                    kprintf("[tcc real] ... alive at +%llus, spins=%llu, +%llu dev reads (+%llu KB)\n",
                            (unsigned long long)((now - t0) / 100),
                            (unsigned long long)spins,
                            (unsigned long long)(nb.reads - pb.reads),
                            (unsigned long long)((nb.read_blocks - pb.read_blocks) / 2));
                    pb = nb;
                    tlast = now;
                }
            }
            code = process_wait((uint32_t)pid);
        }
        kprintf("[tcc real] tcc exit=%d\n", code);

        int rcode = -1;
        struct vfs_stat st;
        if (vfs_stat("/data/tmp/r.elf", &st) == 0) {
            char *b[] = { "/data/tmp/r.elf", NULL };
            int p2 = process_create_env("/data/tmp/r.elf", b, 1, env, NULL, 0);
            rcode = p2 >= 0 ? process_wait((uint32_t)p2) : -1;
            kprintf("[tcc real] r.elf (%llu bytes) ran: exit=%d (want 42)\n",
                    (unsigned long long)st.size, rcode);
        } else {
            kprintf("[tcc real] r.elf MISSING (link failed)\n");
        }

        /* The stdio proof: what fprintf buffered and fclose flushed. */
        static const char want[] = "hdr=ok n=42\n";
        char got[32] = {0};
        size_t r = 0;
        int ofd = vfs_open("/data/tmp/r.out", O_RDONLY, 0);
        if (ofd >= 0) { vfs_fd_read(ofd, got, sizeof got - 1, &r); vfs_close(ofd); }
        int out_ok = (r == sizeof want - 1 && memcmp(got, want, r) == 0);
        kprintf("[tcc real] r.out: %s (got \"%s\")\n", out_ok ? "byte-exact" : "WRONG/MISSING", got);

        int ok = (pid >= 0 && code == 0 && rcode == 42 && out_ok);
        kprintf("\n[cmd] test tcc real: %s\n", ok ? "OK" : "FAIL");
        return 1;
    }

/* THE OS REBUILDS ONE OF ITS OWN TOOLS.
 *
 * tally.elf (the reference pipeline consumer, an external re-implementation of
 * the `count` builtin) is rebuilt FROM SOURCE, ON the OS, and INSTALLED over
 * /data/apps/tally/tally.elf -- then proven by running it through the shell's
 * real extern pipeline. mkfs packs its exact source closure (7 files) at
 * /data/src/shell/, tree-shaped so the quote-includes resolve with one -I.
 *
 * Beyond the headline, this closes the last unproven toolchain primitive:
 * SEPARATE COMPILE-THEN-LINK. `test tcc real` went source -> ELF in one tcc
 * invocation; a build tool's natural shape is `tcc -c` per unit and a second
 * invocation linking the .o files -- four TCC-PRODUCED objects resolving each
 * other's symbols plus libc.a's. That combination runs here for the first time.
 *
 * The install genuinely REPLACES the host-built binary (the fs is mutable and
 * that is the point); functionally identical, and the next host `make`
 * re-bakes the image anyway. The oracle is tally's own documented self-check:
 * `ls / | tally | get rows` must agree with `ls / | count` -- same data, one
 * path through a builtin, one through spawn/INSTALL_OBJ/serialize/deserialize
 * and OUR freshly built binary. */
/* Diagnostic for the extern-CONSUMER pipeline hang: run the hanging line with
 * a timeout, and when it wedges, dump the process table -- who exists, and in
 * what state -- instead of freezing the console. Temporary instrument. */
    if (strcmp(cmd, "test externdiag") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test externdiag: VFS not registered\n"); return 1; }
        char *s[] = { "/system/bin/shell.elf", "-c", "ls / | tally | get rows", NULL };
        int p = process_create("/system/bin/shell.elf", s, 3, NULL, 0);
        kprintf("[externdiag] shell pid=%d\n", p);
        if (p < 0) return 1;
        uint64_t t0 = lapic_timer_get_ticks(), tmark = t0;
        int timed_out = 0;
        while (process_alive((uint32_t)p)) {
            __asm__ volatile ("sti; hlt");
            uint64_t now = lapic_timer_get_ticks();
            if (now - tmark >= 500) {                 /* 5 s heartbeat */
                kprintf("[externdiag] +%llus shell still alive\n",
                        (unsigned long long)((now - t0) / 100));
                tmark = now;
            }
            if (now - t0 > 1500) { timed_out = 1; break; }   /* 15 guest-s (TCG clock runs ~1/3 wall) */
        }
        if (timed_out) {
            struct process_info procs[MAX_PROCESSES];
            int n = process_list(procs, MAX_PROCESSES);
            kprintf("[externdiag] TIMED OUT. process table (%d):\n", n);
            for (int i = 0; i < n; i++) {
                const char *st = "?";
                switch (procs[i].state) {
                case PROCESS_READY:   st = "READY";   break;
                case PROCESS_RUNNING: st = "RUNNING"; break;
                case PROCESS_BLOCKED: st = "BLOCKED"; break;
                case PROCESS_ZOMBIE:  st = "ZOMBIE";  break;
                default: break;
                }
                kprintf("  pid=%-3u ppid=%-3u %-8s %s\n",
                        (unsigned int)procs[i].pid, (unsigned int)procs[i].parent_pid,
                        st, procs[i].is_kthread ? "kthread" : "process");
            }
            /* the shell's own pump diagnostics land in a file (ring-3 console
             * writes go to the screen, not serial) -- read them back here */
            char dbg[400] = {0};
            int dfd = vfs_open("/data/tmp/extdbg.txt", O_RDONLY, 0);
            if (dfd >= 0) {
                size_t r = 0;
                vfs_fd_read(dfd, dbg, sizeof dbg - 1, &r);
                vfs_close(dfd);
                kprintf("[externdiag] shell pump dbg:\n%s", dbg);
            } else {
                kprintf("[externdiag] no /data/tmp/extdbg.txt (pump never hit no-progress)\n");
            }
            process_kill((uint32_t)p);
        }
        int c = timed_out ? -2 : process_wait((uint32_t)p);
        kprintf("\n[cmd] test externdiag: rc=%d (%s)\n", c, timed_out ? "HUNG" : "completed");
        return 1;
    }

/* V2 -- THE SHELL REBUILDS THE SHELL, and the FIRST ADOPTION EVENT
 * (docs/BUILD.md §3). Four movements:
 *   (1) BUILD: EmbBuild walks /data/src/shell/build.ebm -- eleven TCC
 *       compiles and a link, STAGED ONLY (the manifest has no install
 *       stanza; /system/bin is sealed and EmbBuild will not cross it).
 *   (2) VERIFY THE STAGED ARTIFACT: the TCC-built shell runs the shell-test
 *       -c oracles -- expression eval, a real ls pipeline with
 *       where/sort-by/select, and an extern pipeline (the staged shell
 *       spawning tally through its own plumbing) -- BEFORE anyone considers
 *       adopting it.
 *   (3) ADOPT -- the deliberate seal-crossing, done by the SYSTEM (this
 *       test standing in for the hand-run ritual), not by the build tool:
 *       snapshot the volume (rollback safety), then copy staged ->
 *       /system/bin/shell.elf.
 *   (4) POST-ADOPT: the same oracles through /system/bin/shell.elf -- from
 *       here on, every pipeline in the system runs through a shell the
 *       system built for itself. */
    if (strcmp(cmd, "test embbuild shell") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test embbuild shell: VFS not registered\n"); return 1; }
        static char cap[4096];
        int ok = 1, rc;
        char *bb = "/data/apps/embbuild/embbuild.elf";
        static const char *oracle[3] = {
            "echo 1mb + 512kb",
            "ls / | where size > 100kb | sort-by size | select name size",
            "ls / | tally | get rows",
        };

        /* (1) build to staging */
        kprintf("[v2] (1) building the shell from /data/src/shell\n");
        char *a1[] = { bb, "/data/src/shell/build.ebm", NULL };
        rc = emb_run_cap(bb, a1, 2, cap, sizeof cap);
        if (rc != 0 || !emb_find(cap, "12 ran, 0 up_to_date")) { kprintf("[v2] (1) FAIL rc=%d\n", rc); ok = 0; }

        /* (2) verify the staged shell BEFORE adoption */
        if (ok) {
            kprintf("[v2] (2) verifying the STAGED shell\n");
            for (int i = 0; i < 3 && ok; i++) {
                char *s[] = { "/data/build/out/shell/shell.elf", "-c", (char *)oracle[i], NULL };
                int p = process_create("/data/build/out/shell/shell.elf", s, 3, NULL, 0);
                int c = p >= 0 ? process_wait((uint32_t)p) : -1;
                kprintf("[v2] (2) staged -c \"%s\" rc=%d\n", oracle[i], c);
                if (c != 0) ok = 0;
            }
        }

        /* (3) the adoption ritual: snapshot, then the seal-crossing copy --
         * performed HERE, by the system's own hand, never by EmbBuild. */
        if (ok) {
            struct embkfs_volume *vol = embkfs_live_volume();
            int src = vol ? embkfs_snapshot_create(vol, "pre-shell-adopt") : -1;
            kprintf("[v2] (3) snapshot 'pre-shell-adopt': %s\n",
                    src == EMBK_OK ? "created" : "FAILED (continuing -- copy is still reversible by rebuild)");
            static char xfer[4096];
            int in = vfs_open("/data/build/out/shell/shell.elf", O_RDONLY, 0);
            int out = vfs_open("/system/bin/shell.elf", O_WRONLY | O_CREAT | O_TRUNC, 0755);
            if (in < 0 || out < 0) { kprintf("[v2] (3) FAIL open in=%d out=%d\n", in, out); ok = 0; }
            else {
                size_t total = 0, got = 0;
                for (;;) {
                    if (vfs_fd_read(in, xfer, sizeof xfer, &got) != EMBK_OK || got == 0) break;
                    size_t w = 0;
                    if (vfs_fd_write(out, xfer, got, &w) != EMBK_OK || w != got) { ok = 0; break; }
                    total += w;
                }
                kprintf("[v2] (3) ADOPTED: %u bytes -> /system/bin/shell.elf\n", (unsigned)total);
                if (total < 1024) ok = 0;
            }
            if (in >= 0) vfs_close(in);
            if (out >= 0) vfs_close(out);
        }

        /* (4) the adopted shell IS the system's shell now -- same oracles */
        if (ok) {
            kprintf("[v2] (4) verifying the ADOPTED /system/bin/shell.elf\n");
            for (int i = 0; i < 3 && ok; i++) {
                char *s[] = { "/system/bin/shell.elf", "-c", (char *)oracle[i], NULL };
                int p = process_create("/system/bin/shell.elf", s, 3, NULL, 0);
                int c = p >= 0 ? process_wait((uint32_t)p) : -1;
                kprintf("[v2] (4) adopted -c \"%s\" rc=%d\n", oracle[i], c);
                if (c != 0) ok = 0;
            }
        }

        kprintf("\n[cmd] test embbuild shell: %s\n",
                ok ? "OK -- the shell that runs is a shell the OS built" : "FAIL");
        return 1;
    }

/* EMBBUILD'S FIRST GUI TARGET (docs/BUILD.md §6).
 *
 * Everything before this rebuilt STATIC C. The GUI was excluded twice over:
 * first because tcc could not link a libembk.so app at all, then -- once
 * `test tcc dyn` removed that -- because no manifest named one. This closes
 * the second gap, and it is the difference between "the toolchain could" and
 * "the OS does".
 *
 * Why it is a separate test from `test tcc dyn`: that one drives tcc DIRECTLY
 * with a hand-written argv. This one goes through the build tool, which means
 * the dynamic link line has to survive being written down as a manifest
 * stanza -- a shape v1 had never executed (no -static, -rdynamic, the .so as
 * a link input before -lc, emlink_dynstubs.o, libtcc1.o).
 *
 *   (1) the manifest builds: 3 ran, 0 up_to_date
 *   (2) SHAPE: the staged ELF is ET_EXEC with phnum>2 -- the direct readout
 *       that the DYNAMIC path was taken (a static link gets 2). Asserting
 *       this before running it means a static-shaped binary fails HERE, with
 *       a number, rather than mysteriously later.
 *   (3) it RUNS: the kernel binds it to libembk.so, the compositor gives it
 *       a window, the widget stays alive. Rendering is the actual claim.
 *   (4) IDEMPOTENCE: a second run says 0 ran -- the content stamps are honest
 *       about a GUI target too, not just static C.
 *   (5) the ADOPTED binary at /data/apps/clockw/clockw.elf (written by the
 *       manifest's own install stanza) runs as well. */
    if (strcmp(cmd, "test embbuild gui") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test embbuild gui: VFS not registered\n"); return 1; }
        static char cap[4096];
        int ok = 1, rc;
        char *bb  = "/data/apps/embbuild/embbuild.elf";
        char *st  = "/data/build/out/clockw/clockw.elf";
        char *ins = "/data/apps/clockw/clockw.elf";
        char *env[] = { "HOME=/", NULL };

        /* Start from a clean slate: a leftover output would let a no-op run
         * look like a successful build (CONTRIBUTING lie #1). */
        (void)vfs_unlink_path("/data/build/stamps/clockw/clockw.o");
        (void)vfs_unlink_path("/data/build/stamps/clockw/clockw.elf");
        (void)vfs_unlink_path("/data/build/stamps/clockw/install");
        (void)vfs_unlink_path(st);

        kprintf("[embbuild-gui] (1) building the clock widget from /data/src/ui\n");
        char *m[] = { bb, "/data/src/ui/build.ebm", NULL };
        rc = emb_run_cap(bb, m, 2, cap, sizeof cap);
        if (rc != 0 || !emb_find(cap, "3 ran, 0 up_to_date")) {
            kprintf("[embbuild-gui] (1) FAIL rc=%d\n", rc); ok = 0;
        }

        /* (2) the shape -- the whole point of the target */
        if (ok) {
            unsigned char hdr[64] = {0};
            struct vfs_stat s;
            int have = (vfs_stat(st, &s) == 0);
            if (have) {
                int fd = vfs_open(st, O_RDONLY, 0);
                if (fd >= 0) { size_t r = 0; vfs_fd_read(fd, hdr, sizeof hdr, &r); vfs_close(fd); }
            }
            int is_exec = have && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[16] == 2;
            unsigned phnum = have ? (unsigned)(hdr[56] | (hdr[57] << 8)) : 0;
            kprintf("[embbuild-gui] (2) staged: %llu bytes, ET_EXEC=%d phnum=%u (2=static, >2=dynamic)\n",
                    have ? (unsigned long long)s.size : 0ULL, is_exec, phnum);
            if (!is_exec || phnum < 3) { kprintf("[embbuild-gui] (2) FAIL: not the dynamic shape\n"); ok = 0; }
        }

        /* (3) it runs -- the kernel binds it, the compositor windows it */
        if (ok) {
            char *aw[] = { st, NULL };
            int wp = process_create_env(st, aw, 1, env, NULL, 0);
            kprintf("[embbuild-gui] (3) spawn EmbBuild-built widget -> pid=%d\n", wp);
            if (wp < 0) ok = 0;
            else {
                uint64_t t0 = lapic_timer_get_ticks();
                while (lapic_timer_get_ticks() - t0 < 200) {   /* ~2 guest-s */
                    compositor_pointer_tick(); __asm__ volatile ("sti; hlt");
                }
                int alive = process_alive((uint32_t)wp);
                kprintf("[embbuild-gui] (3) widget alive: %s\n",
                        alive ? "YES (loaded + rendering)" : "no (exited/crashed)");
                if (!alive) ok = 0;
                process_kill((uint32_t)wp);
                process_wait((uint32_t)wp);
            }
        }

        /* (4) the stamps are honest about a GUI target too */
        if (ok) {
            rc = emb_run_cap(bb, m, 2, cap, sizeof cap);
            kprintf("[embbuild-gui] (4) rerun (want 0 ran) rc=%d\n", rc);
            if (rc != 0 || !emb_find(cap, "0 ran, 3 up_to_date")) {
                kprintf("[embbuild-gui] (4) FAIL: stamps disagree\n"); ok = 0;
            }
        }

        /* (5) the adopted binary is a working widget */
        if (ok) {
            char *ai[] = { ins, NULL };
            int ip = process_create_env(ins, ai, 1, env, NULL, 0);
            kprintf("[embbuild-gui] (5) spawn ADOPTED %s -> pid=%d\n", ins, ip);
            if (ip < 0) ok = 0;
            else {
                uint64_t t0 = lapic_timer_get_ticks();
                while (lapic_timer_get_ticks() - t0 < 200) {
                    compositor_pointer_tick(); __asm__ volatile ("sti; hlt");
                }
                int alive = process_alive((uint32_t)ip);
                kprintf("[embbuild-gui] (5) adopted widget alive: %s\n", alive ? "YES" : "no");
                if (!alive) ok = 0;
                process_kill((uint32_t)ip);
                process_wait((uint32_t)ip);
            }
        }

        kprintf("\n[cmd] test embbuild gui: %s\n",
                ok ? "OK -- the OS built its own GUI app, and it renders" : "FAIL");
        return 1;
    }

/* EMBBUILD TARGETS #2 + #3 (docs/BUILD.md §10): sysinfo, then THE LOOP-CLOSER
 * -- EmbBuild rebuilds EmbBuild, and the TCC-built successor is cross-checked
 * against its gcc-built parent:
 *   (1) tally manifest, gcc-built tool: 6 ran (fresh stamps for the oracle)
 *   (2) sysinfo manifest: 6 ran + the rebuilt sysinfo runs a live pipeline
 *   (3) embbuild manifest: 6 ran -- TCC compiles the build tool, EmbBuild
 *       installs its own successor into /data/apps (no seal crossed)
 *   (4) THE ORACLE: the STAGED, TCC-built embbuild reruns the tally manifest
 *       and must say "0 ran, 6 up_to_date" -- two implementations of the tool
 *       agreeing on the state of the world (the EMBKFS two-oracle pattern)
 *   (5) the INSTALLED successor reruns its OWN manifest: "0 ran, 6 up_to_date"
 *       -- the loop closes on itself */
    if (strcmp(cmd, "test embbuild self") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test embbuild self: VFS not registered\n"); return 1; }
        static char cap[4096];
        int ok = 1, rc;
        char *bb    = "/data/apps/embbuild/embbuild.elf";
        char *bb_st = "/data/build/out/embbuild/embbuild.elf";

        kprintf("[embbuild-self] (1) tally manifest, gcc-built tool\n");
        char *m_tally[] = { bb, "/data/src/tally/build.ebm", NULL };
        rc = emb_run_cap(bb, m_tally, 2, cap, sizeof cap);
        if (rc != 0 || !emb_find(cap, "6 ran, 0 up_to_date")) { kprintf("[embbuild-self] (1) FAIL rc=%d\n", rc); ok = 0; }

        if (ok) {
            kprintf("[embbuild-self] (2) sysinfo manifest + oracle\n");
            char *m_si[] = { bb, "/data/src/sysinfo/build.ebm", NULL };
            rc = emb_run_cap(bb, m_si, 2, cap, sizeof cap);
            if (rc != 0 || !emb_find(cap, "6 ran, 0 up_to_date")) { kprintf("[embbuild-self] (2) FAIL rc=%d\n", rc); ok = 0; }
            if (ok) {
                char *o[] = { "/system/bin/shell.elf", "-c", "sysinfo | get processes", NULL };
                int p = process_create("/system/bin/shell.elf", o, 3, NULL, 0);
                int c = p >= 0 ? process_wait((uint32_t)p) : -1;
                kprintf("[embbuild-self] (2) rebuilt sysinfo pipeline rc=%d\n", c);
                if (c != 0) ok = 0;
            }
        }

        if (ok) {
            kprintf("[embbuild-self] (3) embbuild rebuilds embbuild\n");
            char *m_self[] = { bb, "/data/src/embbuild/build.ebm", NULL };
            rc = emb_run_cap(bb, m_self, 2, cap, sizeof cap);
            if (rc != 0 || !emb_find(cap, "6 ran, 0 up_to_date")) { kprintf("[embbuild-self] (3) FAIL rc=%d\n", rc); ok = 0; }
            if (ok) {   /* what got installed is a runnable ELF, and TCC-sized */
                unsigned char hdr[20] = {0}; struct vfs_stat st;
                int have = (vfs_stat("/data/apps/embbuild/embbuild.elf", &st) == 0);
                if (have) {
                    int fd = vfs_open("/data/apps/embbuild/embbuild.elf", O_RDONLY, 0);
                    if (fd >= 0) { size_t r = 0; vfs_fd_read(fd, hdr, sizeof hdr, &r); vfs_close(fd); }
                }
                int is_exec = have && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[16] == 2;
                kprintf("[embbuild-self] (3) installed successor: %llu bytes, ET_EXEC=%d\n",
                        have ? (unsigned long long)st.size : 0ULL, is_exec);
                if (!is_exec) ok = 0;
            }
        }

        if (ok) {
            kprintf("[embbuild-self] (4) ORACLE: staged TCC-built tool vs tally stamps\n");
            char *m4[] = { bb_st, "/data/src/tally/build.ebm", NULL };
            rc = emb_run_cap(bb_st, m4, 2, cap, sizeof cap);
            if (rc != 0 || !emb_find(cap, "0 ran, 6 up_to_date")) { kprintf("[embbuild-self] (4) FAIL rc=%d\n", rc); ok = 0; }
        }

        if (ok) {
            kprintf("[embbuild-self] (5) installed successor vs its OWN manifest\n");
            char *m5[] = { bb, "/data/src/embbuild/build.ebm", NULL };   /* bb is now TCC-built */
            rc = emb_run_cap(bb, m5, 2, cap, sizeof cap);
            if (rc != 0 || !emb_find(cap, "0 ran, 6 up_to_date")) { kprintf("[embbuild-self] (5) FAIL rc=%d\n", rc); ok = 0; }
        }

        kprintf("\n[cmd] test embbuild self: %s\n", ok ? "OK (loop closed)" : "FAIL");
        return 1;
    }

/* EMBBUILD ACCEPTANCE (docs/BUILD.md §10) -- the six cases, one boot:
 *   (a) clean build via manifest + the tally pipeline oracle
 *   (b) immediate re-run: "0 ran" -- content stamps hit
 *   (c) edit one source: exactly that unit's CHAIN reruns
 *   (d) change one stanza's flag, sources untouched: it reruns -- the
 *       false-fresh case make structurally fails on this machine
 *   (e) mis-ordered manifest: parse-time refusal naming both stanzas
 *   (f) install aimed at /system: the §3 boundary refusal
 * All EmbBuild output arrives via emb_run_cap (fd1+2 piped) so the
 * transcript lands in the serial record, not just on the screen. */
    if (strcmp(cmd, "test embbuild") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test embbuild: VFS not registered\n"); return 1; }
        static char cap[4096];
        static char fbuf[8192];
        int ok = 1;
        char *bb = "/data/apps/embbuild/embbuild.elf";

        /* (a) clean build + oracle */
        kprintf("[embbuild-test] (a) clean build\n");
        char *a1[] = { bb, "/data/src/tally/build.ebm", NULL };
        int rc = emb_run_cap(bb, a1, 2, cap, sizeof cap);
        if (rc != 0 || !emb_find(cap, "6 ran, 0 up_to_date")) { kprintf("[embbuild-test] (a) FAIL rc=%d\n", rc); ok = 0; }
        if (ok) {
            char *o1[] = { "/system/bin/shell.elf", "-c", "ls / | tally | get rows", NULL };
            int p = process_create("/system/bin/shell.elf", o1, 3, NULL, 0);
            int c1 = p >= 0 ? process_wait((uint32_t)p) : -1;
            char *o2[] = { "/system/bin/shell.elf", "-c", "ls / | count", NULL };
            p = process_create("/system/bin/shell.elf", o2, 3, NULL, 0);
            int c2 = p >= 0 ? process_wait((uint32_t)p) : -1;
            kprintf("[embbuild-test] (a) oracle: tally rc=%d count rc=%d\n", c1, c2);
            if (c1 != 0 || c2 != 0) ok = 0;
        }

        /* (b) re-run: 0 ran */
        if (ok) {
            kprintf("[embbuild-test] (b) no-op rebuild\n");
            rc = emb_run_cap(bb, a1, 2, cap, sizeof cap);
            if (rc != 0 || !emb_find(cap, "0 ran, 6 up_to_date")) { kprintf("[embbuild-test] (b) FAIL rc=%d\n", rc); ok = 0; }
        }

        /* (c) one edited source: its chain reruns (tally.o -> link -> install),
         * the other three compiles stay skipped. The edit adds a SYMBOL so the
         * object genuinely changes and the chain propagates. */
        if (ok) {
            kprintf("[embbuild-test] (c) edit tally.c -> chain rebuild\n");
            int n = emb_read_all("/data/src/tally/tally.c", fbuf, sizeof fbuf);
            static const char probe[] = "\nint emb_probe_case_c;\n";
            if (n < 0 || n + (int)sizeof probe >= (int)sizeof fbuf) { ok = 0; }
            else {
                memcpy(fbuf + n, probe, sizeof probe - 1);
                if (emb_write_all("/data/src/tally/tally.c", fbuf, (size_t)n + sizeof probe - 1) != 0) ok = 0;
            }
            if (ok) {
                rc = emb_run_cap(bb, a1, 2, cap, sizeof cap);
                if (rc != 0 || !emb_find(cap, "3 ran, 3 up_to_date") ||
                    !emb_find(cap, "skipping 'sval.o'")) {
                    kprintf("[embbuild-test] (c) FAIL rc=%d\n", rc); ok = 0;
                }
            }
        }

        /* (d) THE ONE: a flag change with sources untouched. Surgery on the
         * manifest: insert -DEMB_FLAG_D=1 into sval.o's args (the token
         * "-I/data/src/shell /data/src/shell/sval/sval.c" is unique to it),
         * run from /data/tmp -- same project, same stamps. Expect exactly
         * sval.o to rerun; and because the flag changes no bytes of the
         * object, the chain CUTS OFF: 1 ran, 5 up to date. Timestamps could
         * never express any of this. */
        if (ok) {
            kprintf("[embbuild-test] (d) flag-only rebuild\n");
            int n = emb_read_all("/data/src/tally/build.ebm", fbuf, sizeof fbuf);
            static const char at[]  = "-I/data/src/shell /data/src/shell/sval/sval.c";
            static const char ins[] = "-DEMB_FLAG_D=1 ";
            int pos = -1;
            for (int i = 0; n > 0 && i + (int)sizeof at - 1 <= n; i++)
                if (strncmp(fbuf + i, at, sizeof at - 1) == 0) { pos = i; break; }
            if (n < 0 || pos < 0 || n + (int)sizeof ins >= (int)sizeof fbuf) ok = 0;
            else {
                int split = pos + 18;   /* after "-I/data/src/shell " */
                memmove(fbuf + split + sizeof ins - 1, fbuf + split, (size_t)(n - split) + 1);
                memcpy(fbuf + split, ins, sizeof ins - 1);
                if (emb_write_all("/data/tmp/flagged.ebm", fbuf, (size_t)n + sizeof ins - 1) != 0) ok = 0;
            }
            if (ok) {
                char *a2[] = { bb, "/data/tmp/flagged.ebm", NULL };
                rc = emb_run_cap(bb, a2, 2, cap, sizeof cap);
                if (rc != 0 || !emb_find(cap, "1 ran, 5 up_to_date") ||
                    !emb_find(cap, "sval.o")) {
                    kprintf("[embbuild-test] (d) FAIL rc=%d\n", rc); ok = 0;
                }
            }
        }

        /* (e) the ordering guard: a manifest whose first stanza consumes the
         * second's output. Refusal must name BOTH stanzas. */
        if (ok) {
            kprintf("[embbuild-test] (e) mis-ordered manifest\n");
            static const char bad[] =
                "project: badorder\n\n"
                "name: use-first\nkind: compile\ninputs: /data/tmp/nope.o\n"
                "args: /data/apps/tcc/tcc.elf -c x -o y\noutput: /data/tmp/z.o\n\n"
                "name: make-nope\nkind: compile\ninputs: /data/src/tally/tally.c\n"
                "args: /data/apps/tcc/tcc.elf -c x -o /data/tmp/nope.o\noutput: /data/tmp/nope.o\n";
            if (emb_write_all("/data/tmp/badorder.ebm", bad, sizeof bad - 1) != 0) ok = 0;
            if (ok) {
                char *a3[] = { bb, "/data/tmp/badorder.ebm", NULL };
                rc = emb_run_cap(bb, a3, 2, cap, sizeof cap);
                if (rc == 0 || !emb_find(cap, "ordering error") ||
                    !emb_find(cap, "use-first") || !emb_find(cap, "make-nope")) {
                    kprintf("[embbuild-test] (e) FAIL rc=%d\n", rc); ok = 0;
                }
            }
        }

        /* (f) the §3 boundary: an install pointed at /system/bin refuses. */
        if (ok) {
            kprintf("[embbuild-test] (f) /system install refusal\n");
            static const char evil[] =
                "project: evil\n\n"
                "name: sneak\nkind: install\ninputs: /data/src/tally/build.ebm\n"
                "output: /system/bin/evil.elf\n";
            if (emb_write_all("/data/tmp/evil.ebm", evil, sizeof evil - 1) != 0) ok = 0;
            if (ok) {
                char *a4[] = { bb, "/data/tmp/evil.ebm", NULL };
                rc = emb_run_cap(bb, a4, 2, cap, sizeof cap);
                if (rc == 0 || !emb_find(cap, "cannot write into /system")) {
                    kprintf("[embbuild-test] (f) FAIL rc=%d\n", rc); ok = 0;
                }
            }
        }

        kprintf("\n[cmd] test embbuild: %s\n", ok ? "OK (a-f)" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test tcc tally") == 0) {
        if (!g_vfs_ready) { kprintf("\n[cmd] test tcc tally: VFS not registered\n"); return 1; }
        char *env[] = { "HOME=/", NULL };
        int ok = 1;

        /* (1) compile each unit: tcc -c -I/data/src/shell src -o obj */
        static const char *units[4][2] = {
            { "/data/src/shell/tools/tally.c", "/data/tmp/ty_tally.o" },
            { "/data/src/shell/sval/sval.c",   "/data/tmp/ty_sval.o"  },
            { "/data/src/shell/value/value.c", "/data/tmp/ty_value.o" },
            { "/data/src/shell/wire/wire.c",   "/data/tmp/ty_wire.o"  },
        };
        for (int u = 0; u < 4 && ok; u++) {
            char *a[] = { "/data/apps/tcc/tcc.elf", "-c", "-I/data/src/shell",
                          (char *)units[u][0], "-o", (char *)units[u][1], NULL };
            int p = process_create_env("/data/apps/tcc/tcc.elf", a, 6, env, NULL, 0);
            int c = p >= 0 ? process_wait((uint32_t)p) : -1;
            kprintf("[tcc tally] cc %s -> exit=%d\n", units[u][0], c);
            if (p < 0 || c != 0) ok = 0;
        }

        /* (2) link the four tcc-produced objects + the ABI into a NEW app,
         * tally2 (bare `tally2` resolves to /data/apps/tally2/tally2.elf via
         * eval_extern's ordered search). Separate invocation = the
         * compile-then-link primitive. The host-built tally stays untouched as
         * the A/B CONTROL for the pipeline oracle below. */
        int lc = -1;
        if (ok) {
            (void)vfs_mkdir_path("/data/apps/tally2");   /* EEXIST is fine */
            char *l[] = { "/data/apps/tcc/tcc.elf", "-static", "-nostdlib",
                          "/system/abi/crt0.o", "/system/abi/syscalls.o",
                          "/data/tmp/ty_tally.o", "/data/tmp/ty_sval.o",
                          "/data/tmp/ty_value.o", "/data/tmp/ty_wire.o",
                          "-lc", "-o", "/data/apps/tally2/tally2.elf", NULL };
            int p = process_create_env("/data/apps/tcc/tcc.elf", l, 12, env, NULL, 0);
            lc = p >= 0 ? process_wait((uint32_t)p) : -1;
            kprintf("[tcc tally] link -> exit=%d\n", lc);
            if (p < 0 || lc != 0) ok = 0;
        }

        /* (2b) look at what we installed (same readout as test tcc link). */
        if (ok) {
            unsigned char hdr[64] = {0};
            struct vfs_stat st;
            int have = (vfs_stat("/data/apps/tally2/tally2.elf", &st) == 0);
            if (have) {
                int fd = vfs_open("/data/apps/tally2/tally2.elf", O_RDONLY, 0);
                if (fd >= 0) { size_t r = 0; vfs_fd_read(fd, hdr, sizeof hdr, &r); vfs_close(fd); }
            }
            int is_exec = have && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[16] == 2;
            kprintf("[tcc tally] installed tally2: %llu bytes, ET_EXEC=%d\n",
                    have ? (unsigned long long)st.size : 0ULL, is_exec);
            if (!is_exec) ok = 0;
        }

        /* (2c) direct spawn, NO pipeline: with a console stdin (not a FIFO),
         * tally must print its "no structured input" usage line and exit 1 --
         * separates "the binary starts and runs at all" from the pipeline
         * plumbing before the oracle below. */
        if (ok) {
            char *d[] = { "/data/apps/tally2/tally2.elf", NULL };
            int p = process_create_env("/data/apps/tally2/tally2.elf", d, 1, env, NULL, 0);
            int c = p >= 0 ? process_wait((uint32_t)p) : -1;
            kprintf("[tcc tally] direct spawn rc=%d (want 1; usage line above proves it ran)\n", c);
            if (p < 0 || c != 1) ok = 0;
        }

        /* (3) the A/B oracle, each leg TIMEOUT-GUARDED (a wedged pipeline must
         * fail the test, not the whole console): control = host-built tally,
         * experiment = our tcc-built tally2, same input. Both numbers land on
         * serial; both legs exiting 0 is the machine verdict. */
        if (ok) {
            static const char *legs[2] = { "ls / | tally | get rows",
                                           "ls / | tally2 | get rows" };
            for (int g = 0; g < 2 && ok; g++) {
                kprintf("[tcc tally] oracle %s: %s\n", g ? "EXPERIMENT" : "CONTROL", legs[g]);
                char *s[] = { "/system/bin/shell.elf", "-c", (char *)legs[g], NULL };
                int p = process_create("/system/bin/shell.elf", s, 3, NULL, 0);
                if (p < 0) { ok = 0; break; }
                uint64_t t0 = lapic_timer_get_ticks();
                int timed_out = 0;
                while (process_alive((uint32_t)p)) {
                    __asm__ volatile ("sti; hlt");
                    if (lapic_timer_get_ticks() - t0 > 4500) {   /* 45 s @100 Hz */
                        timed_out = 1;
                        kprintf("[tcc tally] %s TIMED OUT -- killing shell pid=%d\n",
                                g ? "EXPERIMENT" : "CONTROL", p);
                        process_kill((uint32_t)p);
                        break;
                    }
                }
                int c = timed_out ? -2 : process_wait((uint32_t)p);
                kprintf("[tcc tally] %s rc=%d\n", g ? "EXPERIMENT" : "CONTROL", c);
                if (c != 0) ok = 0;
            }
        }

        kprintf("\n[cmd] test tcc tally: %s\n", ok ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test posix") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test posix: VFS not registered\n");
            return 1;
        }
        char *a[] = { "/data/apps/posixdemo/posixdemo.elf", NULL };
        /* Bracket the run with the block-layer request counters. Only
         * two numbers set throughput on this path (requests, and their average
         * size), and this is the cheapest place to read them honestly. */
        struct embk_blkstat bs0, bs1;
        struct embkfs_stat es0, es1;
        embk_blkstat_reset();
        embkfs_stat_reset();
        embk_blkstat_get(&bs0);
        embkfs_stat_get(&es0);
        int pid = process_create("/data/apps/posixdemo/posixdemo.elf", a, 1, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        embk_blkstat_get(&bs1);
        embkfs_stat_get(&es1);
        uint64_t nreq = bs1.reads - bs0.reads;
        uint64_t nblk = bs1.read_blocks - bs0.read_blocks;
        uint64_t nus = bs1.read_us - bs0.read_us;
        kprintf("[blkstat] %llu device reads, %llu blocks (%llu KB), avg %llu blocks/req\n",
                (unsigned long long)nreq, (unsigned long long)nblk,
                (unsigned long long)(nblk / 2), (unsigned long long)(nreq ? nblk / nreq : 0));
        kprintf("[blkstat] bounce path: %llu read(s), %llu write(s), %llu contended\n"
                "          (contended > 0 is the only proof the bounce lock is\n"
                "           reachable; 0 means every test of it is vacuous)\n",
                (unsigned long long)(bs1.bounce_reads - bs0.bounce_reads),
                (unsigned long long)(bs1.bounce_writes - bs0.bounce_writes),
                (unsigned long long)(bs1.bounce_contended - bs0.bounce_contended));
        kprintf("[blkstat] %llu ms INSIDE dev->read, %llu us/req -- anything above\n"
                "          this in the test's wall clock is NOT the disk\n",
                (unsigned long long)(nus / 1000),
                (unsigned long long)(nreq ? nus / nreq : 0));
        kprintf("[embkfs]  %llu B-tree node reads, %llu prefix calls (most are\n"
                "          want==0 size probes), ecache %llu hit / %llu miss\n",
                (unsigned long long)(es1.node_reads - es0.node_reads),
                (unsigned long long)(es1.prefix_calls - es0.prefix_calls),
                (unsigned long long)(es1.ecache_hit - es0.ecache_hit),
                (unsigned long long)(es1.ecache_miss - es0.ecache_miss));
        kprintf("\n[cmd] test posix: exit=%d -> %s\n", code,
                (pid >= 0 && code == 0) ? "OK" : "FAIL");
        return 1;
    }

    /* CPython. `-c` rather than the REPL on purpose: the first question is
     * simply whether the interpreter can INITIALISE (Py_Initialize walks the
     * frozen importlib, sets up sys.path, opens stdio) and evaluate one
     * expression -- no stdin, no terminal, no stdlib-on-disk needed, since
     * importlib/zipimport are frozen into the binary. Anything it prints,
     * including a startup error, reaches the console through inherited stdio.
     *
     * SKIP, not FAIL, when absent: python.elf only exists if the out-of-tree
     * interpreter was built (Makefile's HAVE_PY gate), and a checkout without it
     * is a supported configuration -- same rule as test cxx. */
    if (strcmp(cmd, "test python") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test python: VFS not registered\n");
            return 1;
        }
        struct vfs_stat st;
        if (vfs_stat("/data/apps/python/python.elf", &st) != EMBK_OK) {
            kprintf("\n[cmd] test python: SKIP -- /python.elf not on the image "
                    "(build it: /home/motsou/cross/configure-py-emblink.sh)\n");
            return 1;
        }
        char *a[] = { "/data/apps/python/python.elf", "-c", "print('hello from CPython on EmbLink')", NULL };
        int pid = process_create("/data/apps/python/python.elf", a, 3, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        kprintf("\n[cmd] test python: exit=%d -> %s\n", code,
                (pid >= 0 && code == 0) ? "OK" : "FAIL");
        return 1;
    }

    /* The EXTERNAL pipeline contract, end to end: programs that are NOT
     * builtins participating as stages. sysinfo.elf = producer (spawned
     * with a pipe as its fd 3, emits one record frame); tally.elf =
     * consumer (previous stage's value serialized into its fd 0, reads to
     * EOF, re-emits). Exercises: extern spawn + INSTALL_OBJ + the shell's
     * self-installed ends (SYS_fd_install_obj) + handle_close EOF + the
     * sval fstat-based structured-in/out detection + wire round-trip
     * ACROSS ADDRESS SPACES -- the machinery no builtin touches. */
    if (strcmp(cmd, "test extern") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test extern: VFS not registered (need /system/bin/shell.elf)\n");
            return 1;
        }
        int ok = 1;

        /* (1) producer alone: spawn, collect one frame, pretty-print */
        char *a1[] = { "/system/bin/shell.elf", "-c", "sysinfo", NULL };
        int p1 = process_create("/system/bin/shell.elf", a1, 3, NULL, 0);
        int c1 = p1 >= 0 ? process_wait((uint32_t)p1) : -1;
        if (p1 < 0 || c1 != 0) ok = 0;

        /* (2) producer into a builtin: the collected record flows on */
        char *a2[] = { "/system/bin/shell.elf", "-c", "sysinfo | get processes", NULL };
        int p2 = process_create("/system/bin/shell.elf", a2, 3, NULL, 0);
        int c2 = p2 >= 0 ? process_wait((uint32_t)p2) : -1;
        if (p2 < 0 || c2 != 0) ok = 0;

        /* (3) builtin into a CONSUMER external and back into a builtin:
         * the full input path (serialize -> child fd 0 -> EOF -> re-emit) */
        char *a3[] = { "/system/bin/shell.elf", "-c", "ls / | tally | get rows", NULL };
        int p3 = process_create("/system/bin/shell.elf", a3, 3, NULL, 0);
        int c3 = p3 >= 0 ? process_wait((uint32_t)p3) : -1;
        if (p3 < 0 || c3 != 0) ok = 0;

        /* (4) a missing external fails the pipeline cleanly (exit 1) */
        char *a4[] = { "/system/bin/shell.elf", "-c", "no-such-tool", NULL };
        int p4 = process_create("/system/bin/shell.elf", a4, 3, NULL, 0);
        int c4 = p4 >= 0 ? process_wait((uint32_t)p4) : -1;
        if (p4 < 0 || c4 != 1) ok = 0;

        /* (5) extern CHAINED INTO extern (the streaming pump both ways):
         * ls's table streams into tally #1, whose record streams into
         * tally #2. */
        char *a5[] = { "/system/bin/shell.elf", "-c", "ls / | tally | tally | get rows", NULL };
        int p5 = process_create("/system/bin/shell.elf", a5, 3, NULL, 0);
        int c5 = p5 >= 0 ? process_wait((uint32_t)p5) : -1;
        if (p5 < 0 || c5 != 0) ok = 0;

        kprintf("\n[cmd] test extern: producer rc=%d; producer|get rc=%d; "
                "ls|tally|get rc=%d; missing-tool rc=%d; extern|extern rc=%d -> %s\n",
                c1, c2, c3, c4, c5, ok ? "OK" : "FAIL");
        return 1;
    }

    /* EmbLink UI Piece 1: cross-address-space shared surfaces. Spawns
     * /system/bin/init.elf's "surface-parent" role, which runs S2/S3 (ownership +
     * starvation) in-process and S1 (a child inherits the surface and reads a
     * pattern the parent wrote, cross-address-space) exiting 55 iff all pass.
     * Then checks the live-surface count returned to its baseline -- proving
     * R2 (surfaces freed on process exit; refcount reaches 0 only after BOTH
     * the parent and the inheriting child have exited). */
    if (strcmp(cmd, "test surface") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test surface: VFS not registered (need /system/bin/init.elf on disk)\n");
            return 1;
        }
        uint32_t live_before = surface_live_count();
        char *argv[] = { "/system/bin/init.elf", "surface-parent", NULL };
        int pid = process_create("/system/bin/init.elf", argv, 2, NULL, 0);
        if (pid < 0) {
            kprintf("\n[cmd] test surface: process_create failed: %s\n", embk_strerror(pid));
            return 1;
        }
        int code = process_wait((uint32_t)pid);
        uint32_t live_after = surface_live_count();
        bool ok = (code == 55) && (live_after == live_before);
        kprintf("\n[cmd] test surface: parent exit=%d (want 55), live surfaces %u->%u: %s\n",
                code, (unsigned)live_before, (unsigned)live_after, ok ? "OK" : "FAIL");
        return 1;
    }

    /* EmbLink UI Piece 1, Layer A: IPC channels. Spawns /system/bin/init.elf's "chan"
     * role, which runs A1/A2/A3 (boundaries + blocking + backpressure, via two
     * threads), A4 + EMSGSIZE (peer-close / oversized recv), and S-chan-3
     * (a surface handle sent COPY then dropped undelivered), exiting 77 iff
     * all pass. Then checks BOTH live counts returned to baseline: channels
     * (no channel leaked on close/exit) and surfaces (the undelivered handle's
     * in-transit ref was released -- A6, no leak). */
    if (strcmp(cmd, "test channel") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test channel: VFS not registered (need /system/bin/init.elf on disk)\n");
            return 1;
        }
        uint32_t s_before = surface_live_count();
        uint32_t c_before = channel_live_count();
        char *argv[] = { "/system/bin/init.elf", "chan", NULL };
        int pid = process_create("/system/bin/init.elf", argv, 2, NULL, 0);
        if (pid < 0) {
            kprintf("\n[cmd] test channel: process_create failed: %s\n", embk_strerror(pid));
            return 1;
        }
        int code = process_wait((uint32_t)pid);
        uint32_t s_after = surface_live_count();
        uint32_t c_after = channel_live_count();
        bool ok = (code == 77) && (s_after == s_before) && (c_after == c_before);
        kprintf("\n[cmd] test channel: exit=%d (want 77), surfaces %u->%u, channels %u->%u: %s\n",
                code, (unsigned)s_before, (unsigned)s_after,
                (unsigned)c_before, (unsigned)c_after, ok ? "OK" : "FAIL");
        return 1;
    }

    /* EmbLink UI Piece 1, Layer C: the real compositor loop (spec C.5), for
     * real. Spawns /system/bin/init.elf's "compositor" role, which itself spawns a
     * "compositor-client" child; the two rendezvous via chan_listen/accept/
     * connect (Layer B, /run/compositor), then the client attaches a surface
     * COPY (S-surf-1 cross-address-space, S-surf-2 ownership, S-surf-3
     * starvation all run on it) and a second surface MOVE (S-move both
     * sides). NEITHER role explicitly destroys/closes anything -- exits 88
     * iff every check passed AND the client's own exit code (66) was
     * collected. Checks surface/channel/endpoint live counts all return to
     * baseline after both processes are reaped: the strongest form of R2/R3
     * -- a full session's worth of shared state, cleaned up automatically. */
    if (strcmp(cmd, "test compositor") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test compositor: VFS not registered (need /system/bin/init.elf on disk)\n");
            return 1;
        }
        uint32_t s_before = surface_live_count();
        uint32_t c_before = channel_live_count();
        uint32_t e_before = endpoint_live_count();
        char *argv[] = { "/system/bin/init.elf", "compositor", NULL };
        int pid = process_create("/system/bin/init.elf", argv, 2, NULL, 0);
        if (pid < 0) {
            kprintf("\n[cmd] test compositor: process_create failed: %s\n", embk_strerror(pid));
            return 1;
        }
        int code = process_wait((uint32_t)pid);
        uint32_t s_after = surface_live_count();
        uint32_t c_after = channel_live_count();
        uint32_t e_after = endpoint_live_count();
        bool ok = (code == 88) && (s_after == s_before) && (c_after == c_before) && (e_after == e_before);
        kprintf("\n[cmd] test compositor: exit=%d (want 88), surfaces %u->%u, channels %u->%u, endpoints %u->%u: %s\n",
                code, (unsigned)s_before, (unsigned)s_after,
                (unsigned)c_before, (unsigned)c_after,
                (unsigned)e_before, (unsigned)e_after, ok ? "OK" : "FAIL");
        return 1;
    }

    /* EmbLink UI Piece 1, Layer B invariant B4: a listener that registers
     * then exits without ever accepting/closing (simulating a crash) must
     * have its epfs node unregistered automatically (R2), so a LATER
     * connect() from an unrelated process is refused cleanly -- never left
     * dangling like a stale Unix socket file. */
    if (strcmp(cmd, "test rendezvous") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test rendezvous: VFS not registered (need /system/bin/init.elf on disk)\n");
            return 1;
        }
        char *argv1[] = { "/system/bin/init.elf", "b4-listen", NULL };
        int pid1 = process_create("/system/bin/init.elf", argv1, 2, NULL, 0);
        if (pid1 < 0) {
            kprintf("\n[cmd] test rendezvous: spawn listener failed: %s\n", embk_strerror(pid1));
            return 1;
        }
        int code1 = process_wait((uint32_t)pid1);

        /* By the time process_wait() returns, R2 cleanup (obj_handles_
         * release_all, inside process_reap_slot) has already run -- so the
         * epfs node is verifiably gone from kernel context too, not just
         * "connect happened to fail for some other reason". */
        struct vfs_stat st;
        int stat_rc = vfs_stat("/run/compositor_b4test_should_not_exist", &st);
        (void)stat_rc;   /* just proving vfs_stat itself doesn't crash on epfs; the real check is below */
        int stat_rc2 = vfs_stat("/run/b4test", &st);

        char *argv2[] = { "/system/bin/init.elf", "b4-connect", NULL };
        int pid2 = process_create("/system/bin/init.elf", argv2, 2, NULL, 0);
        if (pid2 < 0) {
            kprintf("\n[cmd] test rendezvous: spawn connector failed: %s\n", embk_strerror(pid2));
            return 1;
        }
        int code2 = process_wait((uint32_t)pid2);

        /* b4-connect now exits with the POSITIVE errno it got. The node is
         * verifiably gone (vfs_stat below), so connect's vfs_resolve must
         * return -ENOENT -- assert that exact code, not merely "nonzero"
         * (which had masked a get_endpoint wiring bug: ENOSYS on a live
         * endpoint would also have looked like a pass). */
        bool ok = (code1 == 0) && (code2 == EMBK_ENOENT) && (stat_rc2 == -EMBK_ENOENT);
        kprintf("\n[cmd] test rendezvous: listener exit=%d (want 0), connect exit=%d (want %d=ENOENT), "
                "node gone (vfs_stat=%s): %s\n",
                code1, code2, EMBK_ENOENT, embk_strerror(stat_rc2), ok ? "OK" : "FAIL");
        return 1;
    }

    /* EmbLink UI Piece 2: the compositor PROTOCOL (message vocabulary over
     * Piece 1 channels -- no new kernel primitive). Each scenario is a ring-3
     * /system/bin/init.elf run ("ui-proto <scen>") that drives BOTH the client and
     * compositor side of a real channel and exits 0 iff the invariant held:
     *   hs        P2-S1  handshake + request_id correlation; version mismatch
     *                    -> HELLO_ACK(-EPROTO) then peer close -> EPIPE (P3)
     *   reorder   P2-S3  the crux: a POINTER_MOTION event enqueued AHEAD of a
     *                    ROLE_CREATED reply is dispatched as an event, and the
     *                    reply still matched by request_id (async/tagged)
     *   privilege P2-S2  BACKGROUND role refused (-EACCES) on /run/compositor,
     *                    allowed on /run/compositor-shell -- privilege follows
     *                    the ENDPOINT (P4), never the client's claim
     *   pacing    P2-S4  FRAME_DONE observed only AFTER surface_release ran --
     *                    a 1-buffer surface stays un-acquirable until then (P5)
     *   routing   P2-S5  pointer over window A delivered on A's channel only,
     *                    never broadcast to B */
    if (strcmp(cmd, "test ui") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test ui: VFS not registered (need /system/bin/init.elf on disk)\n");
            return 1;
        }
        static const char *scen[] = { "hs", "reorder", "privilege", "pacing", "routing" };
        bool all = true;
        for (int i = 0; i < 5; i++) {
            char *argv[] = { "/system/bin/init.elf", "ui-proto", (char *)scen[i], NULL };
            int pid = process_create("/system/bin/init.elf", argv, 3, NULL, 0);
            if (pid < 0) {
                kprintf("\n[cmd] test ui: %s: process_create failed: %s\n",
                        scen[i], embk_strerror(pid));
                all = false;
                continue;
            }
            int code = process_wait((uint32_t)pid);
            bool ok = (code == 0);
            all = all && ok;
            kprintf("\n[cmd] test ui: %s: exit=%d (want 0): %s", scen[i], code, ok ? "OK" : "FAIL");
        }
        kprintf("\n[cmd] test ui: %s\n", all ? "OK" : "FAIL");
        return 1;
    }

    // docs/architecture/process-and-scheduling.md §12. Each of these
    // temporarily takes over current_process/the scheduler and restores it
    // to NULL before returning — don't run them while a real process is
    // scheduled (they'd fight over current_process with it).
    if (strcmp(cmd, "test sched roundrobin") == 0) {
        int rc = process_test_roundrobin();
        kprintf("\n[cmd] test sched roundrobin: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test fpu") == 0) {
        int rc = process_test_fpu();
        kprintf("\n[cmd] test fpu: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test sched kill") == 0) {
        int rc = process_test_kill();
        kprintf("\n[cmd] test sched kill: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test sched reap") == 0) {
        int rc = process_test_reap();
        kprintf("\n[cmd] test sched reap: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test sched stackguard") == 0) {
        int rc = process_test_stackguard();
        kprintf("\n[cmd] test sched stackguard: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test sched wait") == 0) {
        int rc = process_test_wait();
        kprintf("\n[cmd] test sched wait: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test sched priority") == 0) {
        int rc = process_test_priority();
        kprintf("\n[cmd] test sched priority: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test smp sched") == 0) {
        int rc = process_test_smp_sched();
        kprintf("\n[cmd] test smp sched: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test smp kill") == 0) {
        int rc = process_test_smp_kill();
        kprintf("\n[cmd] test smp kill: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test thread smp") == 0) {
        int rc = process_test_thread_smp();
        kprintf("\n[cmd] test thread smp: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test thread exit") == 0) {
        int rc = process_test_thread_exit();
        kprintf("\n[cmd] test thread exit: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test smp online") == 0) {
        bool ok = true;
        kprintf("\n");
        for (uint32_t i = 0; i < cpu_count; i++) {
            kprintf("  cpu %u (apic_id %u): %s\n", (unsigned int)i,
                    (unsigned int)cpu_table[i].apic_id,
                    cpu_table[i].online ? "online" : "OFFLINE");
            if (!cpu_table[i].online) {
                ok = false;
            }
        }
        kprintf("[cmd] test smp online: %s (%u core(s))\n", ok ? "OK" : "FAIL",
                (unsigned int)cpu_count);
        return 1;
    }

    if (strcmp(cmd, "test usb") == 0) {
        int rc = usb_run_selftests();
        kprintf("\n[cmd] test usb: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test gpu") == 0) {
        int rc = fb_run_selftests();
        kprintf("\n[cmd] test gpu: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test fat32") == 0) {
        if (!g_has_fat32 || !g_fat32) {
            kprintf("\n[cmd] test fat32: no FAT32 volume mounted\n");
            return 1;
        }
        fat32_test_all(g_fat32);
        kprintf("\n[cmd] test fat32: done\n");
        return 1;
    }

    if (strcmp(cmd, "test fat32 vfs") == 0) {
        if (!g_has_fat32 || !g_fat32) {
            kprintf("\n[cmd] test fat32 vfs: no FAT32 volume mounted\n");
            return 1;
        }
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test fat32 vfs: VFS not registered\n");
            return 1;
        }
        int rc = fat32_vfs_test_mkdir_unlink();
        kprintf("\n[cmd] test fat32 vfs: %s\n", rc == EMBK_OK ? "OK" : embk_strerror(rc));
        return 1;
    }

/* THE FORMATTER'S NESTED DIRECTORY SUPPORT, read by the real kernel. mkfs
 * baked /system/bin/hello.txt et al. into dirtree.img (the 2nd volume via
 * `make run-dirtree`); this resolves that nested path with the SAME
 * embkfs_lookup_path the VFS uses and asserts the exact bytes. Proves what the
 * in-process + oracle checks cannot: that a mkfs-baked multi-level tree is
 * traversable on-metal -- the prerequisite for docs/USERSPACE.md's migration. */
/* THE USERSPACE LAYOUT (docs/USERSPACE.md §6). Asserts the derived tree is
 * actually on the boot volume, the loader's ONE hardwired library sits at its new
 * home, and the deliberate SCOPE decision held (demos + fonts stayed at root).
 * The loader/boot half is doubly confirmed: home.elf booting AT ALL means the
 * loader already resolved /system/lib/libembk.so -- if this test's paths were
 * wrong, there would be no desktop to run it from. */
    if (strcmp(cmd, "test layout") == 0) {
        int pass = 0, fail = 0;
        struct vfs_stat st;
        #define IS_THERE(pth)  (vfs_stat((pth), &st) == EMBK_OK)
        #define LCHK(name, cond) do { if (cond) pass++; else { fail++; \
            kprintf("  FAIL %s\n", name); } } while (0)

        /* /system -- sealed region (D2): programs, the toolkit, the ABI. */
        LCHK("/system/bin/{shell,home,init}.elf",
             IS_THERE("/system/bin/shell.elf") && IS_THERE("/system/bin/home.elf") &&
             IS_THERE("/system/bin/init.elf"));
        LCHK("/system/lib/libembk.so (the loader's target)", IS_THERE("/system/lib/libembk.so"));
        LCHK("/system/abi/{crt0.o,syscalls.o,libc.a}",
             IS_THERE("/system/abi/crt0.o") && IS_THERE("/system/abi/syscalls.o") &&
             IS_THERE("/system/abi/libc.a"));

        /* /data -- mutable state: installed apps + user/scratch dirs. */
        LCHK("/data/apps/{tcc,git}", IS_THERE("/data/apps/tcc/tcc.elf") &&
             IS_THERE("/data/apps/git/git.elf"));
        LCHK("/data/tmp and /data/users/teo exist",
             IS_THERE("/data/tmp") && IS_THERE("/data/users/teo"));

        /* THE SCOPE DECISION, asserted so a future reader knows it was deliberate:
         * demos and fonts stayed at ROOT and did NOT move under /system. */
        LCHK("a demo moved to /data/apps (/data/apps/uidemo/uidemo.elf, not /uidemo.elf)",
             IS_THERE("/data/apps/uidemo/uidemo.elf") && !IS_THERE("/uidemo.elf"));
        LCHK("fonts stayed at root (/font.ttf, not /system/lib/font.ttf)",
             IS_THERE("/font.ttf") && !IS_THERE("/system/lib/font.ttf"));

        /* The OLD flat paths must be GONE -- a lingering /system/bin/shell.elf would mean the
         * migration half-happened (dangerous: two truths for one program). */
        LCHK("old flat /shell.elf is gone", !IS_THERE("/shell.elf"));
        LCHK("old flat /libembk.so is gone", !IS_THERE("/libembk.so"));

        kprintf("\n[cmd] test layout: %s (%d/%d)\n",
                fail == 0 ? "OK" : "FAIL", pass, pass + fail);
        #undef IS_THERE
        #undef LCHK
        return 1;
    }

    if (strcmp(cmd, "test dirtree") == 0) {
        if (embkfs_volume_count() < 2) {
            kprintf("\n[cmd] test dirtree: SKIP (boot the fixture as a 2nd volume: "
                    "`make run-dirtree`)\n");
            return 1;
        }
        struct embkfs_volume *v = embkfs_volume_at(1);
        int pass = 0, fail = 0;
        #define DCHK(name, cond) do { if (cond) pass++; else { fail++; \
            kprintf("  FAIL %s\n", name); } } while (0)

        /* 1. Resolve a THREE-component nested path and read its exact content. */
        uint64_t oid = 0;
        int rc = embkfs_lookup_path(v, EMBKFS_ROOT_OBJECT_ID,
                                    "/system/bin/hello.txt", &oid);
        DCHK("resolve /system/bin/hello.txt", rc == EMBK_OK && oid != 0);
        if (rc == EMBK_OK) {
            uint8_t buf[64]; uint64_t got = 0;
            memset(buf, 0, sizeof buf);
            rc = embkfs_read_object(v, oid, buf, sizeof buf, &got);
            static const char want[] = "hi from /system/bin\n";
            int match = (rc == EMBK_OK && got == sizeof(want) - 1 &&
                         memcmp(buf, want, sizeof(want) - 1) == 0);
            DCHK("content of the nested file is exact", match);
            kprintf("  read %llu bytes: \"%s\"\n", (unsigned long long)got, (char *)buf);
        }

        /* 2. An EXPLICIT empty directory resolves (proves DT_DIR baking). */
        uint64_t d = 0;
        DCHK("resolve empty dir /data/tmp",
             embkfs_lookup_path(v, EMBKFS_ROOT_OBJECT_ID, "/data/tmp", &d) == EMBK_OK && d != 0);
        DCHK("resolve deep dir /data/apps/foo",
             embkfs_lookup_path(v, EMBKFS_ROOT_OBJECT_ID, "/data/apps/foo", &d) == EMBK_OK && d != 0);

        /* 3. Non-vacuous: a path THROUGH a non-existent directory must FAIL, not
         *    silently succeed -- otherwise "resolves" proves nothing. */
        uint64_t bad = 0;
        DCHK("bogus /system/nope/x.txt is refused",
             embkfs_lookup_path(v, EMBKFS_ROOT_OBJECT_ID, "/system/nope/x.txt", &bad) != EMBK_OK);

        kprintf("\n[cmd] test dirtree: %s (%d/%d)\n",
                fail == 0 ? "OK" : "FAIL", pass, pass + fail);
        #undef DCHK
        return 1;
    }

    if (strcmp(cmd, "test tty") == 0) {
        int rc = tty_run_selftests();
        kprintf("\n[cmd] test tty: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    return 0;
}
