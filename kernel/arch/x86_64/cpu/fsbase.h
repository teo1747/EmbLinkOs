#ifndef __FSBASE_H__
#define __FSBASE_H__

#include <stdint.h>

/* FS_BASE -- the x86-64 thread pointer, and thus EmbLink's thread-local storage.
 *
 * A TLS access compiles to `mov %fs:0x0, %reg` (fetch the TCB self-pointer) and
 * then a negative offset off it, so %fs's BASE must point at the thread's TCB.
 * The base is not a normal register: it lives in the IA32_FS_BASE MSR, which is
 * per-CPU, not per-thread. It therefore has to be reinstalled on every context
 * switch, exactly like CR3 and TSS.rsp0 -- see process.c's schedule(), where
 * this sits between tss_set_rsp0() and kernel_ctx_switch().
 *
 * WHY THERE IS NO fsbase_get()/save-on-switch: CR4.FSGSBASE is deliberately NOT
 * enabled (kernel/cpu/fpu.c sets only OSFXSR/OSXMMEXCPT), so the WRFSBASE
 * instruction #UDs in ring 3 and the KERNEL IS THE ONLY WRITER of FS_BASE.
 * struct thread::fs_base is therefore authoritative at all times: we restore it
 * on the way in and never have to read the MSR back out. Enabling FSGSBASE
 * later would let userspace change the base behind our back and would REQUIRE
 * adding a save (rdfsbase) on the way out -- don't enable it casually.
 */
#define MSR_FS_BASE 0xC0000100u

static inline void fsbase_set(uint64_t base) {
    /* wrmsr takes the value split across edx:eax, and writing a 64-bit MSR with
     * a 32-bit-truncated value would silently give the thread a bogus (low-half)
     * thread pointer, so both halves matter. */
    uint32_t low  = (uint32_t)(base & 0xFFFFFFFFu);
    uint32_t high = (uint32_t)(base >> 32);
    __asm__ volatile ("wrmsr" :: "c"(MSR_FS_BASE), "a"(low), "d"(high));
}

#endif /* __FSBASE_H__ */
