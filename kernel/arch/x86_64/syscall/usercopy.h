#ifndef __USERCOPY_H__
#define __USERCOPY_H__

#include <stdint.h>
#include "include/types.h"

// Check that [user_ptr, user_ptr+len) lies entirely within canonical
// low-half (user) address space AND is mapped in the CURRENT process's own
// page tables. Every syscall handler must call this (directly, or via
// copy_from_user/copy_to_user below) before ever dereferencing a pointer a
// ring-3 caller handed it — see docs/TODO.md's "Security — user-pointer
// validation" entry for the hole this closes.
bool access_ok(const void *user_ptr, size_t len);

// Copy `len` bytes from a user-space pointer into a kernel buffer.
// Returns EMBK_OK on success, -EMBK_EFAULT if the range fails access_ok().
int copy_from_user(void *kernel_dst, const void *user_src, size_t len);

/* Retry accounting for the transient-EFAULT workaround. `retries` is the number
 * that matters: it counts validations that FAILED once and succeeded on a later
 * attempt, i.e. the transient actually occurring. Zero of those under real SMP
 * load is the evidence needed to delete the retry loop rather than keep it out
 * of superstition. See the long comment in usercopy.c. */
struct usercopy_stat_pub {
    uint64_t calls;
    uint64_t retries;
    uint64_t faults;
    uint64_t worst_tries;
};
void usercopy_stat_get(struct usercopy_stat_pub *out);
void usercopy_stat_reset(void);

// Copy `len` bytes from a kernel buffer into a user-space pointer.
// Returns EMBK_OK on success, -EMBK_EFAULT if the range fails access_ok().
int copy_to_user(void *user_dst, const void *kernel_src, size_t len);

// Copy a NUL-terminated string from user space into a kernel buffer of
// `max_len` bytes (including the NUL). Validates one byte at a time via
// access_ok() rather than a single fixed-length check, since the string's
// real length isn't known up front. Returns the string length (excluding
// the NUL) on success, -EMBK_EFAULT on an invalid pointer, or
// -EMBK_ENAMETOOLONG if no NUL is found within max_len bytes.
int copy_string_from_user(char *kernel_dst, const char *user_src, size_t max_len);

#endif /* __USERCOPY_H__ */
