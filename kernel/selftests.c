#include "selftests.h"

#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "fs/embkfs/embkfs.h"
#include "block/block.h"   /* blkstat request counters (test ioperf) */
#include "drivers/timer/hpet.h"
#include "drivers/timer/timer.h"
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
    kprintf("  test embkfs timestamps\n");
    kprintf("  test embkfs multivol\n");
    kprintf("  test embkfs compress\n");
    kprintf("  test embkfs selfheal\n");
    kprintf("  test embkfs snapshot\n");
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
        // /init.elf runs the full EmbLink native-primitive suite -- a second
        // thread (create/join + shared-memory proof), a spawn() with argv +
        // file-actions, and an sbrk() heap exercise -- and exits 16 iff ALL
        // of them passed (see user/init.c, built on the EmbLink SDK
        // user/embk.h). 16 is a fixed success sentinel, not a computed value.
        // Needs a real filesystem with /init.elf on it (e.g. `make
        // run-embkfs`) -- unlike "test ring3", there's no embedded fallback
        // blob.
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test ring3 threads: VFS not registered (need /init.elf on disk)\n");
            return 1;
        }
        char *argv[] = { "/init.elf", NULL };
        int pid = process_create("/init.elf", argv, 1, NULL, 0);
        if (pid < 0) {
            kprintf("\n[cmd] test ring3 threads: process_create failed: %s\n", embk_strerror(pid));
            return 1;
        }

        int code = process_wait((uint32_t)pid);
        kprintf("\n[cmd] test ring3 threads: /init.elf exited with code %d (want 16): %s\n",
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
            kprintf("\n[cmd] test handle reap: VFS not registered (need /init.elf)\n");
            return 1;
        }
        struct process *me = current_process;
        int ok = 1;

        /* (1) a LONG-LIVED child ("spin" loops forever); leak its handle. */
        char *aspin[] = { "/init.elf", "spin", NULL };
        int spin_pid = process_create("/init.elf", aspin, 2, NULL, 0);
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
            char *aleak[] = { "/init.elf", "leak", NULL };
            int p = process_create("/init.elf", aleak, 2, NULL, 0);
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
            kprintf("\n[cmd] test pipe: VFS not registered (need /init.elf)\n");
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
        char *aw[] = { "/init.elf", "pipewrite", NULL };
        int wpid = process_create("/init.elf", aw, 2, &act, 1);
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

    /* The structured shell, end to end ON the OS: spawn /shell.elf -c "<line>"
     * three times (its stdio is console-inherited, so results land on serial)
     * and assert the exit codes: expression evaluation, a REAL ls pipeline
     * over EMBKFS through where/sort-by/select, and the error path. The pure
     * pipeline is host-tested (make shell-test); THIS proves the OS half:
     * spawn, console fds, readdir/stat, and the exit-status plumbing. */
    if (strcmp(cmd, "test shell") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test shell: VFS not registered (need /shell.elf)\n");
            return 1;
        }
        int ok = 1;

        /* env: the shell must SHOW the environment it was handed, and `env set`
         * must reach the same `environ` it passes to the commands it spawns.
         * Spawned WITH an environment here, so the rendered table proves the
         * whole chain: process_create_env -> RDX -> crt0 -> environ -> builtin. */
        /* `|`, not `;` -- the lexer has no statement separator. Builtins run
         * IN-PROCESS, so the set and the listing share one `environ`. */
        char *aenv[] = { "/shell.elf", "-c", "env set EMBK_SET fromshell | env", NULL };
        char *senv[] = { "EMBK_ENV_TEST=shell-ok", "HOME=/", NULL };
        int pe = process_create_env("/shell.elf", aenv, 3, senv, NULL, 0);
        int ce = pe >= 0 ? process_wait((uint32_t)pe) : -1;
        if (pe < 0 || ce != 0) ok = 0;

        char *a1[] = { "/shell.elf", "-c", "echo 1mb + 512kb", NULL };
        int p1 = process_create("/shell.elf", a1, 3, NULL, 0);
        int c1 = p1 >= 0 ? process_wait((uint32_t)p1) : -1;
        if (p1 < 0 || c1 != 0) ok = 0;

        char *a2[] = { "/shell.elf", "-c",
                       "ls / | where size > 100kb | sort-by size | select name size", NULL };
        int p2 = process_create("/shell.elf", a2, 3, NULL, 0);
        int c2 = p2 >= 0 ? process_wait((uint32_t)p2) : -1;
        if (p2 < 0 || c2 != 0) ok = 0;

        char *a3[] = { "/shell.elf", "-c", "echo $nope", NULL };
        int p3 = process_create("/shell.elf", a3, 3, NULL, 0);
        int c3 = p3 >= 0 ? process_wait((uint32_t)p3) : -1;
        if (p3 < 0 || c3 != 1) ok = 0;   /* the error path must exit 1 */

        /* the standard-command batch, one line, RE-RUNNABLE (rm cleans up
         * what save created; no mkdir -- there's no rmdir to undo it):
         * save -> cat -> wc -> get -> rm -> ps -> count, with a cd/pwd
         * warm-up. Exercises the cwd machinery + file round-trip + proc
         * listing in a single shell run. */
        char *a4[] = { "/shell.elf", "-c",
                       "cd / | echo \"alpha beta\" | save st-f.txt | cat /st-f.txt "
                       "| wc | get words | rm st-f.txt | mkdir st-d | rmdir st-d "
                       "| ps | count", NULL };
        int p4 = process_create("/shell.elf", a4, 3, NULL, 0);
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
        if (vfs_stat("/cxxdemo.elf", &st) != EMBK_OK) {
            kprintf("\n[cmd] test cxx: SKIP -- /cxxdemo.elf not on the image "
                    "(no C++ toolchain: see `make cxx-check`)\n");
            return 1;
        }
        char *a[] = { "/cxxdemo.elf", NULL };
        int pid = process_create("/cxxdemo.elf", a, 1, NULL, 0);
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
        char *a[] = { "/posixdemo.elf", NULL };
        /* EMBK_EMPTY is set-but-empty on purpose: getenv() must return "" for it,
         * not NULL -- "set to nothing" and "not set" are different answers. */
        char *env[] = {
            "EMBK_ENV_TEST=1",
            "HOME=/",
            "PATH=/",
            "EMBK_EMPTY=",
            NULL
        };
        int pid = process_create_env("/posixdemo.elf", a, 1, env, NULL, 0);
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

        kprintf("\n[cmd] test ksync: %s (%d/%d)\n",
                fail == 0 ? "OK" : "FAIL", pass, pass + fail);
        #undef SCHK
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
        char *a[] = { "/init.elf", "ctrlc-parent", NULL };
        int pid = process_create("/init.elf", a, 2, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        kprintf("\n[cmd] test ctrlc: exit=%d -> %s\n", code,
                (pid >= 0 && code == 42) ? "OK" : "FAIL");
        return 1;
    }

    /* The ESCALATION half (docs/INTERRUPTION.md §4.3), against a child that
     * DECLINES: the shell runs `/init.elf spin` (an embk_sleep_ms loop that
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
        char *a[] = { "/shell.elf", "-c", "/init.elf spin", NULL };
        int pid = process_create("/shell.elf", a, 3, NULL, 0);
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
        char *a[] = { "/git.elf", "version", NULL };
        char *env[] = { "HOME=/", NULL };
        int pid = process_create_env("/git.elf", a, 2, env, NULL, 0);
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
            { "init",   { "/git.elf", "init" }, 2 },
            { "email",  { "/git.elf", "config", "user.email", "motsou@emblink" }, 4 },
            { "name",   { "/git.elf", "config", "user.name", "Motsou" }, 4 },
            { "add",    { "/git.elf", "add", "README.md" }, 3 },
            { "commit", { "/git.elf", "commit", "-m", "first commit on EmbLinkOS" }, 4 },
            { "log",    { "/git.elf", "log", "--oneline" }, 3 },
            { "status", { "/git.elf", "status" }, 2 },
        };
        for (unsigned i = 0; i < sizeof steps / sizeof steps[0] && ok; i++) {
            int pid = process_create_env("/git.elf", steps[i].argv, steps[i].argc,
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
            { "init",   { "/git.elf", "init" }, 2 },
            { "email",  { "/git.elf", "config", "user.email", "motsou@emblink" }, 4 },
            { "name",   { "/git.elf", "config", "user.name", "Motsou" }, 4 },
            { "add",    { "/git.elf", "add", "hello.txt" }, 3 },   /* RELATIVE */
            { "commit", { "/git.elf", "commit", "-m", "committed from /proj" }, 4 },
            { "status", { "/git.elf", "status", "--short" }, 3 },
        };
        for (unsigned i = 0; i < sizeof steps / sizeof steps[0] && ok; i++) {
            int pid = process_create_env("/git.elf", steps[i].argv, steps[i].argc,
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

        char *a1[] = { "/shell.elf", "-c", "cd /cwdshell | pwd", NULL };
        int p1 = process_create_env("/shell.elf", a1, 3, env, NULL, 0);
        int c1 = p1 >= 0 ? process_wait((uint32_t)p1) : -1;
        kprintf("[shell cd|pwd] exit=%d\n", c1);
        if (p1 < 0 || c1 != 0) ok = 0;

        /* cd, then SPAWN git there. The spawn is the point: a builtin sharing
         * the shell's own cwd proves nothing about what a CHILD sees. */
        char *a2[] = { "/shell.elf", "-c", "cd /cwdshell | git init", NULL };
        int p2 = process_create_env("/shell.elf", a2, 3, env, NULL, 0);
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
        char *a[] = { "/tcc.elf", "-v", NULL };
        char *env[] = { "HOME=/", NULL };
        int pid = process_create_env("/tcc.elf", a, 2, env, NULL, 0);
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
            int fd = vfs_open("/t.c", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { kprintf("test tcc compile: cannot write /t.c\n"); return 1; }
            size_t w = 0;
            vfs_fd_write(fd, src, sizeof src - 1, &w);
            vfs_close(fd);
        }
        (void)vfs_unlink_path("/t.o");

        char *a[] = { "/tcc.elf", "-c", "/t.c", "-o", "/t.o" };
        char *env[] = { "HOME=/", NULL };
        int pid = process_create_env("/tcc.elf", a, 5, env, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        kprintf("[tcc -c] exit=%d\n", code);

        /* Read the object back and look at it. */
        unsigned char hdr[20] = {0};
        struct vfs_stat st;
        int have = (vfs_stat("/t.o", &st) == 0);
        if (have) {
            int fd = vfs_open("/t.o", O_RDONLY, 0);
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
            int fd = vfs_open("/l.c", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { kprintf("test tcc link: cannot write /l.c\n"); return 1; }
            size_t w = 0;
            vfs_fd_write(fd, src, sizeof src - 1, &w);
            vfs_close(fd);
        }
        (void)vfs_unlink_path("/l.elf");

        char *a[] = { "/tcc.elf", "-static", "-nostdlib", "/l.c", "/crt0.o",
                      "/syscalls.o", "-L/", "-lc", "-o", "/l.elf" };
        char *env[] = { "HOME=/", NULL };
        /* Echo the command. It documents what is being asked, and it doubles as
         * a staleness canary: if this line does not appear, you are not running
         * the kernel you just built -- check that BEFORE doubting the compiler. */
        kprintf("[tcc link] %s %s %s %s %s %s %s %s %s\n",
                a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]);
        int pid = process_create_env("/tcc.elf", a, 10, env, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        kprintf("[tcc link] exit=%d\n", code);

        /* Read enough of the header to see e_phnum. It is the DIRECT readout of
         * whether -static took: tccelf.c gives a static exe 2 program headers
         * and a dynamic one 5 (PHDR+INTERP+2xLOAD+DYNAMIC). Measuring it here,
         * in the kernel, beats extracting the file and guessing on the host. */
        unsigned char hdr[64] = {0};
        struct vfs_stat st;
        int have = (vfs_stat("/l.elf", &st) == 0);
        if (have) {
            int fd = vfs_open("/l.elf", O_RDONLY, 0);
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
            char *b[] = { "/l.elf", NULL };
            int p2 = process_create_env("/l.elf", b, 1, env, NULL, 0);
            rcode = p2 >= 0 ? process_wait((uint32_t)p2) : -1;
            kprintf("[tcc link] /l.elf ran: exit=%d (want 42)\n", rcode);
        }

        int ok = (pid >= 0 && code == 0 && is_elf && is_exec && rcode == 42);
        kprintf("\n[cmd] test tcc link: %s\n", ok ? "OK" : "FAIL");
        return 1;
    }

    if (strcmp(cmd, "test posix") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test posix: VFS not registered\n");
            return 1;
        }
        char *a[] = { "/posixdemo.elf", NULL };
        /* Bracket the run with the block-layer request counters. Only
         * two numbers set throughput on this path (requests, and their average
         * size), and this is the cheapest place to read them honestly. */
        struct embk_blkstat bs0, bs1;
        struct embkfs_stat es0, es1;
        embk_blkstat_reset();
        embkfs_stat_reset();
        embk_blkstat_get(&bs0);
        embkfs_stat_get(&es0);
        int pid = process_create("/posixdemo.elf", a, 1, NULL, 0);
        int code = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        embk_blkstat_get(&bs1);
        embkfs_stat_get(&es1);
        uint64_t nreq = bs1.reads - bs0.reads;
        uint64_t nblk = bs1.read_blocks - bs0.read_blocks;
        uint64_t nus = bs1.read_us - bs0.read_us;
        kprintf("[blkstat] %llu device reads, %llu blocks (%llu KB), avg %llu blocks/req\n",
                (unsigned long long)nreq, (unsigned long long)nblk,
                (unsigned long long)(nblk / 2), (unsigned long long)(nreq ? nblk / nreq : 0));
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
        if (vfs_stat("/python.elf", &st) != EMBK_OK) {
            kprintf("\n[cmd] test python: SKIP -- /python.elf not on the image "
                    "(build it: /home/motsou/cross/configure-py-emblink.sh)\n");
            return 1;
        }
        char *a[] = { "/python.elf", "-c", "print('hello from CPython on EmbLink')", NULL };
        int pid = process_create("/python.elf", a, 3, NULL, 0);
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
            kprintf("\n[cmd] test extern: VFS not registered (need /shell.elf)\n");
            return 1;
        }
        int ok = 1;

        /* (1) producer alone: spawn, collect one frame, pretty-print */
        char *a1[] = { "/shell.elf", "-c", "sysinfo", NULL };
        int p1 = process_create("/shell.elf", a1, 3, NULL, 0);
        int c1 = p1 >= 0 ? process_wait((uint32_t)p1) : -1;
        if (p1 < 0 || c1 != 0) ok = 0;

        /* (2) producer into a builtin: the collected record flows on */
        char *a2[] = { "/shell.elf", "-c", "sysinfo | get processes", NULL };
        int p2 = process_create("/shell.elf", a2, 3, NULL, 0);
        int c2 = p2 >= 0 ? process_wait((uint32_t)p2) : -1;
        if (p2 < 0 || c2 != 0) ok = 0;

        /* (3) builtin into a CONSUMER external and back into a builtin:
         * the full input path (serialize -> child fd 0 -> EOF -> re-emit) */
        char *a3[] = { "/shell.elf", "-c", "ls / | tally | get rows", NULL };
        int p3 = process_create("/shell.elf", a3, 3, NULL, 0);
        int c3 = p3 >= 0 ? process_wait((uint32_t)p3) : -1;
        if (p3 < 0 || c3 != 0) ok = 0;

        /* (4) a missing external fails the pipeline cleanly (exit 1) */
        char *a4[] = { "/shell.elf", "-c", "no-such-tool", NULL };
        int p4 = process_create("/shell.elf", a4, 3, NULL, 0);
        int c4 = p4 >= 0 ? process_wait((uint32_t)p4) : -1;
        if (p4 < 0 || c4 != 1) ok = 0;

        /* (5) extern CHAINED INTO extern (the streaming pump both ways):
         * ls's table streams into tally #1, whose record streams into
         * tally #2. */
        char *a5[] = { "/shell.elf", "-c", "ls / | tally | tally | get rows", NULL };
        int p5 = process_create("/shell.elf", a5, 3, NULL, 0);
        int c5 = p5 >= 0 ? process_wait((uint32_t)p5) : -1;
        if (p5 < 0 || c5 != 0) ok = 0;

        kprintf("\n[cmd] test extern: producer rc=%d; producer|get rc=%d; "
                "ls|tally|get rc=%d; missing-tool rc=%d; extern|extern rc=%d -> %s\n",
                c1, c2, c3, c4, c5, ok ? "OK" : "FAIL");
        return 1;
    }

    /* EmbLink UI Piece 1: cross-address-space shared surfaces. Spawns
     * /init.elf's "surface-parent" role, which runs S2/S3 (ownership +
     * starvation) in-process and S1 (a child inherits the surface and reads a
     * pattern the parent wrote, cross-address-space) exiting 55 iff all pass.
     * Then checks the live-surface count returned to its baseline -- proving
     * R2 (surfaces freed on process exit; refcount reaches 0 only after BOTH
     * the parent and the inheriting child have exited). */
    if (strcmp(cmd, "test surface") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test surface: VFS not registered (need /init.elf on disk)\n");
            return 1;
        }
        uint32_t live_before = surface_live_count();
        char *argv[] = { "/init.elf", "surface-parent", NULL };
        int pid = process_create("/init.elf", argv, 2, NULL, 0);
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

    /* EmbLink UI Piece 1, Layer A: IPC channels. Spawns /init.elf's "chan"
     * role, which runs A1/A2/A3 (boundaries + blocking + backpressure, via two
     * threads), A4 + EMSGSIZE (peer-close / oversized recv), and S-chan-3
     * (a surface handle sent COPY then dropped undelivered), exiting 77 iff
     * all pass. Then checks BOTH live counts returned to baseline: channels
     * (no channel leaked on close/exit) and surfaces (the undelivered handle's
     * in-transit ref was released -- A6, no leak). */
    if (strcmp(cmd, "test channel") == 0) {
        if (!g_vfs_ready) {
            kprintf("\n[cmd] test channel: VFS not registered (need /init.elf on disk)\n");
            return 1;
        }
        uint32_t s_before = surface_live_count();
        uint32_t c_before = channel_live_count();
        char *argv[] = { "/init.elf", "chan", NULL };
        int pid = process_create("/init.elf", argv, 2, NULL, 0);
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
     * real. Spawns /init.elf's "compositor" role, which itself spawns a
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
            kprintf("\n[cmd] test compositor: VFS not registered (need /init.elf on disk)\n");
            return 1;
        }
        uint32_t s_before = surface_live_count();
        uint32_t c_before = channel_live_count();
        uint32_t e_before = endpoint_live_count();
        char *argv[] = { "/init.elf", "compositor", NULL };
        int pid = process_create("/init.elf", argv, 2, NULL, 0);
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
            kprintf("\n[cmd] test rendezvous: VFS not registered (need /init.elf on disk)\n");
            return 1;
        }
        char *argv1[] = { "/init.elf", "b4-listen", NULL };
        int pid1 = process_create("/init.elf", argv1, 2, NULL, 0);
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

        char *argv2[] = { "/init.elf", "b4-connect", NULL };
        int pid2 = process_create("/init.elf", argv2, 2, NULL, 0);
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
     * /init.elf run ("ui-proto <scen>") that drives BOTH the client and
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
            kprintf("\n[cmd] test ui: VFS not registered (need /init.elf on disk)\n");
            return 1;
        }
        static const char *scen[] = { "hs", "reorder", "privilege", "pacing", "routing" };
        bool all = true;
        for (int i = 0; i < 5; i++) {
            char *argv[] = { "/init.elf", "ui-proto", (char *)scen[i], NULL };
            int pid = process_create("/init.elf", argv, 3, NULL, 0);
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

    if (strcmp(cmd, "test tty") == 0) {
        int rc = tty_run_selftests();
        kprintf("\n[cmd] test tty: %s\n", rc == 0 ? "OK" : "FAIL");
        return 1;
    }

    return 0;
}
