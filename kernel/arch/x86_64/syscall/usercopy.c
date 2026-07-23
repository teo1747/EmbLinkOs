#include "include/kprintf.h"
#include "arch/x86_64/syscall/usercopy.h"
#include "process/process.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "include/errno.h"
#include "include/kstring.h"

// Canonical low-half / user-space ceiling. Every real user mapping in this
// kernel (ELF image, user stack) lives well below this. Kernel VA space
// (direct map, MMIO, heap, kernel image) all lives at 0xFFFF... and above,
// so bounding the raw pointer here already excludes it by address alone —
// the vmm_get_phys_in() walk below additionally confirms the range is
// actually mapped, not just numerically plausible.
#define USER_VA_LIMIT 0x0000800000000000ULL

/* Read THIS thread's address-space root atomically w.r.t. preemption.
 *
 * THE TRANSIENT-EFAULT ROOT CAUSE (was open; this is the fix). Both
 * `current_thread` and `current_process` expand to PER-CPU state reached
 * through this_cpu(), which resolves the core by reading the LAPIC ID:
 *
 *     #define current_thread  (this_cpu()->current_thread)
 *     #define current_process (current_thread->proc)
 *
 * A syscall runs with IF=1 (syscall_dispatch sti's, deliberately -- see its
 * comment). So a timer IRQ could land BETWEEN this_cpu() resolving a core
 * and the dereference of what it returned. If the scheduler then resumed
 * this thread ON A DIFFERENT CORE, the already-computed &cpu_table[old]
 * still sat in a register -- and the deref read whatever OTHER thread was
 * by then running on the old core, yielding a FOREIGN pml4. access_ok then
 * validated our pointer against someone else's address space and reported a
 * genuinely-mapped page as absent: a spurious EFAULT.
 *
 * That is exactly why the symptom was SMP-ONLY (one core can't migrate) and
 * "transient" -- it needs a preemption inside a few-instruction window. It
 * is the same root cause as the ledgered sys_read byte-drop (copy_to_user's
 * false EFAULT after the fs already consumed bytes).
 *
 * The fix is to make the per-CPU read atomic (IF=0 across it) and then keep
 * only its RESULT. pml4_phys is a property of the PROCESS, not of the core,
 * so once captured it stays valid for the whole walk even if this thread
 * migrates a microsecond later. Returns 0 if there's no process context. */
static uint64_t current_pml4_atomic(void) {
    /* The IF=0 per-CPU read now lives in process.h, shared with every other
     * site that needs it (sys_sbrk hit the identical bug). */
    struct process *proc = current_process_atomic();
    return proc ? proc->pml4_phys : 0;
}

bool access_ok(const void *user_ptr, size_t len) {
    if (len == 0) {
        return true;   // nothing to touch, vacuously fine
    }

    uint64_t addr = (uint64_t)(uintptr_t)user_ptr;
    uint64_t end = addr + len;
    if (end < addr) {
        return false;   // integer overflow: the range wrapped past UINT64_MAX
    }
    if (end > USER_VA_LIMIT) {
        return false;   // spills into (or sits entirely in) kernel VA space
    }

    /* Capture ONCE, atomically -- never re-read per-CPU state per page. */
    uint64_t pml4 = current_pml4_atomic();
    if (!pml4) {
        return false;   // no process context to validate against
    }

    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last_page = (end - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (; page <= last_page; page += PAGE_SIZE) {
        if (!vmm_get_phys_in(pml4, page)) {
            return false;   // a page in range isn't mapped in this process
        }
    }
    return true;
}

/* THE RETRY IS GONE. It used to be 8 attempts, papering over a transient
 * false-absent from the page-walk that nobody had root-caused.
 *
 * Two real fixes landed after that workaround, both in this exact path:
 *   - access_ok stopped re-reading per-CPU state per page and now captures the
 *     pml4 ONCE with IF=0 (the migration race: this_cpu() resolved a core, a
 *     timer IRQ migrated the thread, the deref returned a FOREIGN pml4);
 *   - vmm_get_phys_in took vmm_lock, closing the multi-level walk against a
 *     concurrent mapper -- the compositor installing shared window pages into
 *     a client's PML4 while that client walked it ("font.ttf open failed -14").
 *
 * Measured before removing it (`test usercopy`, -smp 4, both halves of the
 * original repro running at once: 6 readers hammering the syscall boundary
 * while UI launches force the compositor to map into client PML4s mid-flight):
 *
 *     3496 validations, 0 transient retries, deepest 1 attempt
 *
 * WHY REMOVE RATHER THAN KEEP IT "just in case". A retry loop is not free
 * insurance -- it is a silencer. It cannot tell a transient from the first
 * symptom of a NEW bug, so anything it hides stays hidden for months, which is
 * precisely how this one survived long enough to become "residual, unexplained".
 * One attempt, and a failure is now LOUD and counted: if the transient ever
 * returns it announces itself with the address that failed, which is the thing
 * a diagnosis actually needs.
 *
 * The honest caveat: zero occurrences over one run is EVIDENCE, not proof. If
 * this line starts printing, the counters are still here and re-arming the
 * retry is a one-line change -- but do the diagnosis first this time. */
#define USERCOPY_ATTEMPTS 1

/* IS THE RETRY STILL DOING ANYTHING? That question has to be answerable before
 * the loop can be removed OR before the "root cause still open" note above can
 * be believed, because TWO real fixes have landed since it was written:
 *
 *   - access_ok stopped re-reading per-CPU state per page and now captures the
 *     pml4 once with IF=0 (the migration race: this_cpu() resolved a core, a
 *     timer IRQ migrated the thread, the deref returned a FOREIGN pml4);
 *   - vmm_get_phys_in took vmm_lock, closing the multi-level-walk race against
 *     a concurrent mapper (the compositor installing shared window pages into a
 *     client's PML4 while that client walked it -- "font.ttf open failed -14").
 *
 * Either could have been the whole of it. So count instead of assume:
 *   retries  - a first attempt failed and a later one succeeded. This is the
 *              ONLY evidence the transient still exists. Zero of these under
 *              load means the loop is dead code kept by superstition.
 *   faults   - all attempts failed: a genuinely bad pointer, the case the
 *              function is actually for.
 * Reported by `test usercopy`. */
static struct usercopy_stat {
    uint64_t calls;
    uint64_t retries;      /* transient false-absents that cleared on retry */
    uint64_t faults;       /* exhausted every attempt -- a real bad pointer  */
    uint64_t worst_tries;  /* most attempts any single call ever needed      */
} g_ucstat;

void usercopy_stat_get(struct usercopy_stat_pub *out) {
    if (!out) return;
    out->calls       = g_ucstat.calls;
    out->retries     = g_ucstat.retries;
    out->faults      = g_ucstat.faults;
    out->worst_tries = g_ucstat.worst_tries;
}
void usercopy_stat_reset(void) {
    g_ucstat.calls = g_ucstat.retries = g_ucstat.faults = g_ucstat.worst_tries = 0;
}

/* The shared validate-with-retry, so both directions count identically. */
static bool access_ok_counted(const void *p, size_t len) {
    g_ucstat.calls++;
    for (int t = 0; t < USERCOPY_ATTEMPTS; t++) {
        if (access_ok(p, len)) {
            if ((uint64_t)(t + 1) > g_ucstat.worst_tries) g_ucstat.worst_tries = (uint64_t)(t + 1);
            if (t > 0) g_ucstat.retries++;      /* the transient, caught in the act */
            return true;
        }
    }
    g_ucstat.faults++;
    /* Loud, and only on the first few, so a genuinely bad userspace pointer in
     * a loop cannot drown the log. A bad pointer from a buggy program is the
     * expected case; the same message from a program known to pass valid ones
     * is the transient returning, and now it says so instead of being retried
     * away in silence. */
    if (g_ucstat.faults <= 8)
        kprintf("usercopy: access_ok REFUSED %p len %lu (fault #%lu) -- bad user pointer, "
                "or the SMP transient is back; see usercopy.c\n",
                p, (unsigned long)len, (unsigned long)g_ucstat.faults);
    return false;
}

int copy_from_user(void *kernel_dst, const void *user_src, size_t len) {
    if (!access_ok_counted(user_src, len)) return -EMBK_EFAULT;
    memcpy(kernel_dst, user_src, len);
    return EMBK_OK;
}

int copy_to_user(void *user_dst, const void *kernel_src, size_t len) {
    if (!access_ok_counted(user_dst, len)) return -EMBK_EFAULT;
    memcpy(user_dst, kernel_src, len);
    return EMBK_OK;
}

int copy_string_from_user(char *kernel_dst, const char *user_src, size_t max_len) {
    /* Validate one PAGE at a time, not one byte: access_ok now takes vmm_lock,
     * so a per-byte check was a lock cycle per character. Re-validate only when
     * the next byte crosses into a new page. */
    uint64_t validated_page = ~0ULL;
    for (size_t i = 0; i < max_len; i++) {
        uint64_t byte_page = ((uint64_t)(uintptr_t)(user_src + i)) & ~(uint64_t)(PAGE_SIZE - 1);
        if (byte_page != validated_page) {
            if (!access_ok_counted(user_src + i, 1)) return -EMBK_EFAULT;
            validated_page = byte_page;
        }
        char c = user_src[i];
        kernel_dst[i] = c;
        if (c == '\0') return (int)i;
    }
    return -EMBK_ENAMETOOLONG;
}
