#include "usercopy.h"
#include "../process/process.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../include/errno.h"
#include "../include/kstring.h"

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
    if (!current_process) {
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

int copy_from_user(void *kernel_dst, const void *user_src, size_t len) {
    if (!access_ok(user_src, len)) {
        return -EMBK_EFAULT;
    }
    memcpy(kernel_dst, user_src, len);
    return EMBK_OK;
}

int copy_to_user(void *user_dst, const void *kernel_src, size_t len) {
    if (!access_ok(user_dst, len)) {
        return -EMBK_EFAULT;
    }
    memcpy(user_dst, kernel_src, len);
    return EMBK_OK;
}

int copy_string_from_user(char *kernel_dst, const char *user_src, size_t max_len) {
    for (size_t i = 0; i < max_len; i++) {
        char c;
        if (copy_from_user(&c, user_src + i, 1) != EMBK_OK) {
            return -EMBK_EFAULT;
        }
        kernel_dst[i] = c;
        if (c == '\0') {
            return (int)i;
        }
    }
    return -EMBK_ENAMETOOLONG;
}
