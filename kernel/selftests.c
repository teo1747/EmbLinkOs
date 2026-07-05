#include "selftests.h"

#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "fs/embkfs/embkfs.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "cpu/rwlock.h"
#include "cpu/usermode.h"
#include "process/process.h"
#include "drivers/usb.h"
#include "drivers/framebuffer.h"

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
    kprintf("  test embkfs ns\n");
    kprintf("  test embkfs all\n");
    kprintf("  test embkfs diag\n");
    kprintf("  test vfs\n");
    kprintf("  test fd\n");
    kprintf("  test fat32\n");
    kprintf("  test fat32 vfs\n");
    kprintf("  test rwlock\n");
    kprintf("  test ring3\n");
    kprintf("  test sched roundrobin\n");
    kprintf("  test sched kill\n");
    kprintf("  test sched reap\n");
    kprintf("  test sched stackguard\n");
    kprintf("  test usb\n");
    kprintf("  test gpu\n");
}

static void run_embkfs_all(void)
{
    int rc_path = embkfs_run_path_selftests();
    int rc_alloc = embkfs_run_allocator_selftests();
    int rc_tree = embkfs_run_tree_selftests();
    int rc_obj = embkfs_run_object_selftests();
    int rc_ns = embkfs_run_namespace_selftests();

    if (rc_path == EMBK_OK && rc_alloc == EMBK_OK && rc_tree == EMBK_OK && rc_obj == EMBK_OK && rc_ns == EMBK_OK) {
        kprintf("\n[cmd] test embkfs all: OK\n");
        return;
    }

    if (rc_path != EMBK_OK)  kprintf("\n[cmd] embkfs path failed: %s\n", embk_strerror(rc_path));
    if (rc_alloc != EMBK_OK) kprintf("\n[cmd] embkfs alloc failed: %s\n", embk_strerror(rc_alloc));
    if (rc_tree != EMBK_OK)  kprintf("\n[cmd] embkfs tree failed: %s\n", embk_strerror(rc_tree));
    if (rc_obj != EMBK_OK)   kprintf("\n[cmd] embkfs obj failed: %s\n", embk_strerror(rc_obj));
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

    if (strcmp(cmd, "test ring3") == 0) {
        // Drop to ring 3, run the user stub (write + exit via int 0x80), and
        // return here when it exits. Re-runnable: enter_user_mode now tears
        // down the process address space on both exit and load failure.
        enter_user_mode();
        kprintf("\n[cmd] test ring3: returned to kernel\n");
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
