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
    if (!current_thread) {
        return false;   // no process context to validate against
    }

    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last_page = (end - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (; page <= last_page; page += PAGE_SIZE) {
        if (!vmm_get_phys_in(current_process->pml4_phys, page)) {
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
