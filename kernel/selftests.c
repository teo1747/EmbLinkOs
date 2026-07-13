#include "selftests.h"

#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "fs/embkfs/embkfs.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "arch/x86_64/cpu/rwlock.h"
#include "drivers/timer/rtc.h"
#include "arch/x86_64/syscall/usermode.h"
#include "process/process.h"
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

static struct fat32_volume *g_fat32 = NULL;
static bool g_has_fat32 = false;
static bool g_vfs_ready = false;

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

    return 0;
}
