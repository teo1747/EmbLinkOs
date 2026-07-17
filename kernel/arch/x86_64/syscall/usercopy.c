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

/* access_ok can fail TRANSIENTLY under -smp (a rare false-absent from the
 * page-walk on a genuinely mapped page; root cause still open -- see the
 * sys-read-silent-byte-drop memory). The transient clears immediately, so
 * retry the validation a handful of times before reporting a real fault:
 * this keeps every syscall's user-copy robust without asking userspace to
 * retry spurious EFAULTs. */
#define USERCOPY_RETRIES 8

int copy_from_user(void *kernel_dst, const void *user_src, size_t len) {
    for (int t = 0; t < USERCOPY_RETRIES; t++) {
        if (access_ok(user_src, len)) {
            memcpy(kernel_dst, user_src, len);
            return EMBK_OK;
        }
    }
    return -EMBK_EFAULT;
}

int copy_to_user(void *user_dst, const void *kernel_src, size_t len) {
    for (int t = 0; t < USERCOPY_RETRIES; t++) {
        if (access_ok(user_dst, len)) {
            memcpy(user_dst, kernel_src, len);
            return EMBK_OK;
        }
    }
    return -EMBK_EFAULT;
}

int copy_string_from_user(char *kernel_dst, const char *user_src, size_t max_len) {
    /* Validate one PAGE at a time, not one byte: access_ok now takes vmm_lock,
     * so a per-byte check was a lock cycle per character. Re-validate only when
     * the next byte crosses into a new page. */
    uint64_t validated_page = ~0ULL;
    for (size_t i = 0; i < max_len; i++) {
        uint64_t byte_page = ((uint64_t)(uintptr_t)(user_src + i)) & ~(uint64_t)(PAGE_SIZE - 1);
        if (byte_page != validated_page) {
            int ok = 0;
            for (int t = 0; t < USERCOPY_RETRIES && !ok; t++)
                ok = access_ok(user_src + i, 1);
            if (!ok) return -EMBK_EFAULT;
            validated_page = byte_page;
        }
        char c = user_src[i];
        kernel_dst[i] = c;
        if (c == '\0') return (int)i;
    }
    return -EMBK_ENAMETOOLONG;
}
