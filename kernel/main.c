#include <stdint.h>
#include "include/types.h"
#include "include/kprintf.h"
#include "include/io.h"
#include "include/errno.h"
#include "include/kstring.h"

#include "drivers/char/serial.h"
#include "drivers/video/framebuffer.h"
#include "drivers/video/gpu.h"
#include "drivers/video/console.h"
#include "drivers/input/keyboard.h"
#include "drivers/input/mouse.h"
#include "gfx/compositor.h"   /* compositor_pointer_tick() -- cursor/focus/drag */
#include "drivers/timer/timer.h"
#include "drivers/timer/hpet.h"
#include "drivers/timer/rtc.h"
#include "drivers/bus/pci.h"
#include "drivers/usb/usb.h"
#include "drivers/storage/ata.h"
#include "drivers/storage/ahci.h"
#include "drivers/video/bootanim.h"

#include "arch/x86_64/cpu/gdt.h"
#include "arch/x86_64/cpu/percpu.h"
#include "arch/x86_64/smp/smp.h"
#include "arch/x86_64/irq/idt.h"
#include "arch/x86_64/syscall/syscall.h"
#include "arch/x86_64/irq/pic.h"
#include "arch/x86_64/irq/irq.h"
#include "arch/x86_64/irq/lapic.h"
#include "arch/x86_64/irq/ioapic.h"
#include "arch/x86_64/cpu/fpu.h"

#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"

#include "acpi/acpi.h"
#include "block/block.h"
#include "block/partition.h"
#include "fs/fat32.h"
#include "fs/embkfs/embkfs.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "fs/epfs.h"
#include "selftests.h"

#include "process/process.h"


extern uint64_t lapic_timer_get_ticks(void);

/* --------------------------------------------------------------------
 * Interactive process control (run/ps/kill/wait/nice). The kernel's own
 * shell loop below is itself a real, schedulable `current_process` (see
 * process_adopt_current()'s comment) rather than a privileged one-way
 * hand-off, so these commands are just direct calls into process.c's
 * kernel-internal API -- no capability-handle indirection needed here the
 * way cpu/syscall.c's sys_spawn/sys_wait/sys_kill need it, since this code
 * is trusted kernel code, not a sandboxed ring-3 caller.
 * -------------------------------------------------------------------- */

/* If `cmd` starts with `prefix` followed by either a space or the end of
 * the string, return a pointer to the first non-space character after it
 * (possibly the terminating NUL, if there were no arguments). NULL if
 * `cmd` doesn't start with `prefix` at all, or is a different, longer
 * command that merely happens to start with the same letters (e.g. "ps"
 * must not match a "psomething" command that doesn't exist). */
static const char *shell_match_prefix(const char *cmd, const char *prefix)
{
    size_t len = strlen(prefix);
    if (strncmp(cmd, prefix, len) != 0) {
        return NULL;
    }
    if (cmd[len] != '\0' && cmd[len] != ' ') {
        return NULL;
    }
    const char *rest = cmd + len;
    while (*rest == ' ') {
        rest++;
    }
    return rest;
}

/* Minimal unsigned base-10 parser -- no libc, and pids/priorities are
 * always small non-negative numbers typed at this shell, so anything more
 * than "stop at the first non-digit" is unneeded complexity. */
static uint32_t shell_parse_uint(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

static const char *process_state_name(enum process_state s)
{
    switch (s) {
        case PROCESS_UNUSED:  return "UNUSED";
        case PROCESS_READY:   return "READY";
        case PROCESS_RUNNING: return "RUNNING";
        case PROCESS_BLOCKED: return "BLOCKED";
        case PROCESS_ZOMBIE:  return "ZOMBIE";
        default:              return "?";
    }
}

/* Returns true if `cmd` was one of the process-control commands (handled,
 * whether or not it succeeded) so the caller doesn't also try the
 * selftests dispatcher or print "unknown command". */
static bool shell_handle_process_command(const char *cmd)
{
    const char *arg;

    if ((arg = shell_match_prefix(cmd, "run")) != NULL) {
        if (!arg[0]) {
            kprintf("\n[run] usage: run <path>\n");
            return true;
        }
        char *argv[] = { (char *)arg, NULL };
        int pid = process_create(arg, argv, 1, NULL, 0);
        if (pid < 0) {
            kprintf("\n[run] failed to start %s: %s\n", arg, embk_strerror(pid));
        } else {
            kprintf("\n[run] started %s as pid %d\n", arg, pid);
        }
        return true;
    }

    if (strcmp(cmd, "ps") == 0) {
        struct process_info procs[MAX_PROCESSES];
        int n = process_list(procs, MAX_PROCESSES);
        kprintf("\nPID  PPID STATE   PRI KIND    EXIT\n");
        for (int i = 0; i < n; i++) {
            kprintf("%-4u %-4u %-7s %-3u %-7s %d\n",
                    (unsigned int)procs[i].pid, (unsigned int)procs[i].parent_pid,
                    process_state_name(procs[i].state),
                    (unsigned int)procs[i].priority,
                    procs[i].is_kthread ? "kthread" : "process",
                    procs[i].exit_code);
        }
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "kill")) != NULL) {
        if (!arg[0]) {
            kprintf("\n[kill] usage: kill <pid>\n");
            return true;
        }
        uint32_t pid = shell_parse_uint(arg);
        process_kill(pid);
        kprintf("\n[kill] sent to pid %u\n", (unsigned int)pid);
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "wait")) != NULL) {
        if (!arg[0]) {
            kprintf("\n[wait] usage: wait <pid>\n");
            return true;
        }
        uint32_t pid = shell_parse_uint(arg);
        kprintf("\n[wait] blocking for pid %u...\n", (unsigned int)pid);
        int code = process_wait(pid);
        if (code < 0) {
            kprintf("[wait] pid %u: %s\n", (unsigned int)pid, embk_strerror(code));
        } else {
            kprintf("[wait] pid %u exited with code %d\n", (unsigned int)pid, code);
        }
        return true;
    }

    if ((arg = shell_match_prefix(cmd, "nice")) != NULL) {
        uint32_t pid = shell_parse_uint(arg);
        const char *prio_arg = arg;
        while (*prio_arg && *prio_arg != ' ') {
            prio_arg++;
        }
        while (*prio_arg == ' ') {
            prio_arg++;
        }
        if (!arg[0] || !prio_arg[0]) {
            kprintf("\n[nice] usage: nice <pid> <priority 0-3, 0=highest>\n");
            return true;
        }
        uint32_t prio = shell_parse_uint(prio_arg);
        int rc = process_set_priority(pid, (uint8_t)prio);
        if (rc != 0) {
            kprintf("\n[nice] pid %u: %s\n", (unsigned int)pid, embk_strerror(rc));
        } else {
            kprintf("\n[nice] pid %u priority set to %u\n", (unsigned int)pid, (unsigned int)prio);
        }
        return true;
    }

    return false;
}

/* `snap create|list|delete|rollback <name>` -- the shell surface for v2.2
 * Phase 5b's snapshots. Own dispatcher (not folded into
 * shell_handle_process_command) since it's a distinct feature area, but
 * follows the exact same shell_match_prefix() pattern. */
static bool shell_handle_snapshot_command(const char *cmd)
{
    const char *rest = shell_match_prefix(cmd, "snap");
    if (!rest) return false;

    struct embkfs_volume *vol = embkfs_live_volume();
    if (!vol) {
        kprintf("\n[snap] no EMBKFS volume mounted\n");
        return true;
    }

    const char *arg;
    if ((arg = shell_match_prefix(rest, "create")) != NULL) {
        if (!arg[0]) { kprintf("\n[snap] usage: snap create <name>\n"); return true; }
        int rc = embkfs_snapshot_create(vol, arg);
        if (rc != EMBK_OK) kprintf("\n[snap] create '%s' failed: %s\n", arg, embk_strerror(rc));
        else kprintf("\n[snap] created '%s'\n", arg);
        return true;
    }
    if ((arg = shell_match_prefix(rest, "delete")) != NULL) {
        if (!arg[0]) { kprintf("\n[snap] usage: snap delete <name>\n"); return true; }
        int rc = embkfs_snapshot_delete(vol, arg);
        if (rc != EMBK_OK) kprintf("\n[snap] delete '%s' failed: %s\n", arg, embk_strerror(rc));
        else kprintf("\n[snap] deleted '%s'\n", arg);
        return true;
    }
    if ((arg = shell_match_prefix(rest, "rollback")) != NULL) {
        if (!arg[0]) { kprintf("\n[snap] usage: snap rollback <name>\n"); return true; }
        int rc = embkfs_snapshot_rollback(vol, arg);
        if (rc != EMBK_OK) kprintf("\n[snap] rollback '%s' failed: %s\n", arg, embk_strerror(rc));
        else kprintf("\n[snap] rolled back to '%s'\n", arg);
        return true;
    }
    if (strcmp(rest, "list") == 0) {
        struct embk_snapshot_item items[EMBKFS_MAX_SNAPSHOTS];
        uint32_t n = 0;
        embkfs_snapshot_list(vol, items, EMBKFS_MAX_SNAPSHOTS, &n);
        kprintf("\n[snap] %u snapshot%s:\n", (unsigned int)n, n == 1 ? "" : "s");
        for (uint32_t i = 0; i < n; i++) {
            char name[33];
            memcpy(name, items[i].name, 32);
            name[32] = '\0';
            kprintf("  %-31s gen %-6lu %lu ns\n", name,
                    (unsigned long)items[i].generation, (unsigned long)items[i].timestamp);
        }
        return true;
    }

    kprintf("\n[snap] usage: snap create|list|delete|rollback <name>\n");
    return true;
}

/* `stat <path>` -- the ls-adjacent surface for v2.2 Phase 5c's process-
 * provenance tracking (writer_pid), alongside the timestamps Phase 0
 * already stores. Root-relative path only (matches ls's own convention
 * elsewhere in this shell). */
static bool shell_handle_stat_command(const char *cmd)
{
    const char *arg = shell_match_prefix(cmd, "stat");
    if (!arg) return false;

    struct embkfs_volume *vol = embkfs_live_volume();
    if (!vol) { kprintf("\n[stat] no EMBKFS volume mounted\n"); return true; }
    if (!arg[0]) { kprintf("\n[stat] usage: stat <path>\n"); return true; }

    uint64_t oid = 0;
    int rc = embkfs_lookup_path(vol, EMBKFS_ROOT_OBJECT_ID, arg, &oid);
    if (rc != EMBK_OK) { kprintf("\n[stat] %s: %s\n", arg, embk_strerror(rc)); return true; }

    struct embk_inode_item ino;
    rc = embkfs_stat_object(vol, oid, &ino);
    if (rc != EMBK_OK) { kprintf("\n[stat] %s: %s\n", arg, embk_strerror(rc)); return true; }

    kprintf("\n[stat] %s (object %lu)\n", arg, oid);
    kprintf("  size %lu  blocks %lu  links %lu  mode 0x%x\n",
            (unsigned long)ino.size, (unsigned long)ino.blocks, (unsigned long)ino.links,
            (unsigned int)ino.mode);
    kprintf("  atime %lu  mtime %lu  ctime %lu  btime %lu  (ns since epoch)\n",
            (unsigned long)ino.atime, (unsigned long)ino.mtime,
            (unsigned long)ino.ctime, (unsigned long)ino.btime);
    uint32_t wpid = embk_inode_writer_pid(&ino);
    if (wpid) kprintf("  writer_pid %u\n", (unsigned int)wpid);
    else      kprintf("  writer_pid unknown (predates process-provenance, or written by mkfs)\n");
    return true;
}

static void kernel_handle_line_command(const char *cmd)
{
    if (shell_handle_process_command(cmd))
        return;

    if (shell_handle_snapshot_command(cmd))
        return;

    if (shell_handle_stat_command(cmd))
        return;

    if (selftests_handle_command(cmd))
        return;

    if (cmd[0])
        kprintf("\n[cmd] unknown command: %s\n", cmd);
}

void kernel_main(void) {
    // --- Core init ---
    serial_init();
    gdt_init_bsp();   // this_cpu()/percpu_init_topology() aren't usable yet
                      // (need ACPI+LAPIC below) -- operates on cpu_table[0]
                      // (the BSP) directly, see gdt_init_bsp()'s comment.
    idt_init();
    syscall_init();   // install int 0x80 (DPL3) + #DF on IST1; needs idt_init first
    pic_init();
    irq_install();
    fpu_init_this_cpu();   // CR0/CR4 are per-core -- every AP repeats this in
                            // ap_main() (smp.c). Must land before this core's
                            // first kernel_ctx_switch() ever runs an FXSAVE/
                            // FXRSTOR (process.c's schedule_locked()).

    // --- Memory ---
    pmm_init();
    vmm_init();
    kheap_init();
    ap_bootstrap_map();   // permanent low-1MB identity map, needed before
                          // any AP can be started (see smp.h's comment)

    // --- Interrupt controllers (ACPI -> LAPIC -> IO-APIC) ---
    acpi_init();
    lapic_init();
    // this_cpu() becomes usable core-wide from here on: needs both ACPI's
    // MADT CPU list (acpi_init, just above) and a working lapic_get_id()
    // (lapic_init, just above) to identify which cpu_table[] entry is "us".
    percpu_init_topology();
    process_init();       // MUST run before smp_bringup(): each AP's
                          // ap_main() adopts a PCB out of proc_table
                          // (process_adopt_current()) as part of its own
                          // bring-up. process_init() blanket-resets every
                          // slot to PROCESS_UNUSED -- running it AFTER the
                          // APs are up (as an earlier version did, from
                          // the old single-core spot right before the
                          // shell's own adoption below) silently WIPED the
                          // APs' already-adopted idle PCBs: each AP's
                          // current_process kept pointing at a slot the
                          // allocator now considered free and happily
                          // recycled into test kthreads -- two cores
                          // executing "the same" PCB, the root of a whole
                          // family of intermittent SMP corruption.
    smp_bringup();        // start every other core found in the MADT;
                          // each one parks in its own idle loop (ap_main,
                          // smp.c) as a real, adopted scheduler
                          // participant with its own LAPIC timer
    ioapic_init();

    // --- HPET: must come after acpi_init + vmm (for MMIO map) ---
    hpet_init();

    // --- RTC: port I/O only, no MMIO dependency -- the one source of
    // real calendar time this kernel has (EMBKFS inode timestamps and
    // everything downstream of them read this, not LAPIC/HPET/PIT, which
    // only ever count elapsed ticks, not wall-clock date/time). ---
    kprintf("\n=== RTC init ===\n");
    kprintf("RTC: current time (Unix epoch seconds) = %lu\n",
            (unsigned long)rtc_now_unix());

    // --- Devices ---
    
    pci_init();
    usb_init();
    ata_init();    // registers ATA drives as block devices internally
    ahci_init();   // runs IDENTIFY per port, stores sector counts

    // Register AHCI drives as block devices (after ahci_init filled sector counts)
    ahci_register_block_devices();



    // --- Display + input ---
    gpu_init();   // pick VirtIO-GPU / Bochs DISPI before the fb comes up
    fb_init();
    console_init();
    keyboard_init();
    ioapic_route(1, 33, 0, false);   // keyboard GSI 1 -> vector 33 -> CPU 0
    // Clamp the cursor to the ACTUAL screen size (varies by GPU: virtio-gpu is
    // 1280x800, stdvga 1024x768) so it can reach every corner of the desktop.
    {
        const fb_info_t *fbi = fb_get_info();
        mouse_init(fbi ? (int)fbi->width : 1024, fbi ? (int)fbi->height : 768);
    }
    ioapic_route(12, 44, 0, false);  // mouse GSI 12 -> vector 44 -> CPU 0

    // --- Timer (LAPIC) + retire PIC ---
    lapic_timer_init(48);
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    // --- TSC calibration (uses HPET if available, else PIT fallback) ---
    tsc_calibrate();

    __asm__ volatile ("sti");

    // --- Boot splash ---
    boot_animation();
    console_clear();

// ============================================================
    //  Block device enumeration
    // ============================================================
    // Probe each whole disk for an MBR and expose its partitions (sda1, sda2,
    // ...) as block devices. Must run after `sti` above: the ATA read path is
    // IRQ-driven and would hang waiting on an interrupt that can't fire yet.
    // Done before enumeration/mount so partitions appear in the listing and the
    // mount probe below sees them alongside whole disks.
    embk_partition_scan_all();

    kprintf("\n=== Block devices ===\n");
    for (uint32_t i = 0; i < embk_block_count(); i++) {
        struct embk_block_device *dev = embk_block_get(i);
        kprintf("  %s: %u blocks (%u KB)\n",
                dev->name,
                (unsigned int)dev->block_count,
                (unsigned int)((dev->block_count * dev->block_size) / 1024));
    }

    // ============================================================
    //  Mount FAT32 (probe every disk, mount the first valid one)
    // ============================================================
    embkfs_init();
    static struct fat32_volume vol;
    bool found = false;
    for (uint32_t i = 0; i < embk_block_count(); i++) {
        struct embk_block_device *d = embk_block_get(i);
        if (fat32_mount(d, &vol) == EMBK_OK) {
            found = true;
            break;
        }
    }
    selftests_init(&vol, found);
    if (!found) {
        kprintf("No FAT32 volume found on any disk\n");
    }

    vfs_init();

    // EmbLink UI Piece 1, Layer B: the RAM-backed endpoint filesystem, mounted
    // at /run independent of whatever real storage was found above -- IPC
    // rendezvous (chan_listen/connect) shouldn't depend on a disk existing.
    epfs_init();
    {
        int rc = epfs_vfs_register("/run");
        if (rc != EMBK_OK) {
            kprintf("VFS: epfs register at /run failed: %s\n", embk_strerror(rc));
        }
    }

    bool vfs_ready = false;
    struct embkfs_volume *embk_live = embkfs_live_volume();

    if (embk_live) {
        int rc = embkfs_vfs_register("/", embk_live);
        if (rc != EMBK_OK) {
            kprintf("VFS: EMBKFS register failed: %s\n", embk_strerror(rc));
        } else {
            vfs_ready = true;
        }
    }

    // v2.2 (Phase 1): register every EMBKFS volume BEYOND the primary
    // (embkfs_init() now mounts up to EMBKFS_MAX_VOLUMES, not just one --
    // see that function's comment) at its own mount point, one per
    // underlying block device name. Index 0 is embk_live (already
    // registered at "/" above); this only runs for index 1+, so it's a
    // complete no-op on a machine with exactly one EMBKFS volume, which
    // is every machine before this phase and most after it.
    for (uint32_t vi = 1; vi < embkfs_volume_count(); vi++) {
        struct embkfs_volume *extra = embkfs_volume_at(vi);
        if (!extra) continue;
        char mp[64] = "/";
        strcat(mp, extra->dev->name);   // e.g. "/sdb" -- device names are
                                         // short (BLOCK_NAME_LEN) and this
                                         // buffer has ample headroom
        int rc = embkfs_vfs_register(mp, extra);
        if (rc != EMBK_OK) {
            kprintf("VFS: EMBKFS register at %s failed: %s\n", mp, embk_strerror(rc));
        } else {
            kprintf("VFS: EMBKFS volume %s mounted at %s\n", extra->dev->name, mp);
        }
    }

    if (found) {
        const char *fat_mp = vfs_ready ? "/fat32" : "/";
        int rc = fat32_vfs_register(fat_mp, &vol);
        if (rc != EMBK_OK) {
            kprintf("VFS: FAT32 register at %s failed: %s\n", fat_mp, embk_strerror(rc));
        } else {
            vfs_ready = true;
        }
    }

    if (!vfs_ready)
        kprintf("VFS: no filesystem mounted\n");

    selftests_set_vfs_ready(vfs_ready);
    if (vfs_ready)
        vfs_fd_init();

    vfs_ls("/");

    // Turn THIS execution context -- the interactive shell below -- into a
    // real, schedulable process (docs/architecture/process-and-scheduling.md
    // §17/process_adopt_current()'s comment), instead of the old design
    // where main.c auto-launched exactly one hardcoded process
    // (process_start_first(), a ONE-WAY hand-off that never returned here
    // at all). Now the shell is just another round-robin participant:
    // `run <path>` spawns real children as siblings, preemption keeps
    // giving the shell its own turns back, and `ps`/`kill`/`wait` manage
    // whatever's running, all without the shell itself ever exiting.
    // (process_init() itself already ran much earlier -- BEFORE
    // smp_bringup(), see the comment there for why that ordering is
    // load-bearing.)
    if (!process_adopt_current()) {
        kprintf("\nfailed to start the process subsystem -- run/ps/kill/wait unavailable\n");
    }

    // One dedicated idle kthread per core, pinned to it, at the lowest
    // priority band: the guaranteed always-available switch target that
    // lets ANY core immediately get off a dying or blocking process's
    // stack even when nothing else is runnable -- see
    // process_create_idle_for_cpu()'s comment (process.c) for the
    // liveness argument. Created only now (not at smp_bringup time)
    // because kthread creation needs the full VMM/heap up for stacks.
    for (uint32_t ci = 0; ci < cpu_count; ci++) {
        if (!process_create_idle_for_cpu(ci)) {
            kprintf("warning: no idle kthread for cpu %u\n", (unsigned int)ci);
        }
    }

    // Land the user in the graphical HOME launcher: a ring-3 app that takes the
    // whole screen as the compositor's desktop layer and launches other apps
    // (see user/bin/home.c). It runs as an ordinary round-robin sibling of this
    // shell context -- the loop below keeps pumping the compositor pointer and
    // USB, and the serial/keyboard REPL stays available as a debug console.
    {
        char *hargv[] = { (char *)"/home.elf", NULL };
        int hpid = process_create("/home.elf", hargv, 1, NULL, 0);
        if (hpid < 0)
            kprintf("\nhome: failed to launch /home.elf: %s\n", embk_strerror(hpid));
        else
            kprintf("\nhome: launched /home.elf as pid %d\n", hpid);
    }

    // Main loop: keyboard echo + tick heartbeat + process control shell
    uint64_t last = 0;
    char cmd_buf[128];
    uint32_t cmd_len = 0;
    for (;;) {
        uint64_t now = lapic_timer_get_ticks();
        if (now >= last + 500) { last = now; }
        // Legacy USB HCs (UHCI/OHCI/EHCI) are polled: drain any completed
        // interrupt-IN transfers and re-arm them. xHCI input is IRQ-driven.
        usb_poll();
        // Drive the window compositor's pointer: cursor, click-to-focus, and
        // title-bar drag. Runs here (schedulable shell-process context) so the
        // compositor spinlock is never taken from an IRQ handler. No-op until a
        // window exists.
        compositor_pointer_tick();
        // USB HID input now arrives via the xHCI interrupt (see xhci_enable_irq),
        // which wakes us from the hlt below and injects keys into the keyboard buffer.
        if (!keyboard_is_grabbed() && keyboard_has_char()) {
            char c = keyboard_getchar();
            kprintf("%c", c);

            if (c == '\r' || c == '\n') {
                cmd_buf[cmd_len] = '\0';
                kernel_handle_line_command(cmd_buf);
                cmd_len = 0;
            } else if ((c == '\b' || c == 127) && cmd_len > 0) {
                cmd_len--;
            } else if (c >= 32 && c <= 126) {
                if (cmd_len + 1 < sizeof cmd_buf)
                    cmd_buf[cmd_len++] = c;
            }
        }

        __asm__ volatile ("hlt");   // wake on any IRQ (timer, PS/2, or xHCI)
    }
}