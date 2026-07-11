#include "arch/x86_64/cpu/fpu.h"

#include <stdint.h>

/* See fpu.h for why this must be called once per core. Without it,
 * CR4.OSFXSR stays clear and every SSE instruction -- this kernel's own
 * kernel_ctx_switch FXSAVE/FXRSTOR included, once that lands -- faults with
 * #UD (invalid opcode), not a degraded/emulated mode. */
void fpu_init_this_cpu(void) {
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);   // CR0.EM = 0 -- no FPU emulation, we have a real one
    cr0 |=  (1ULL << 1);   // CR0.MP = 1 -- WAIT/FWAIT honors CR0.TS (not used
                            //               here -- eager save/restore, no
                            //               lazy switching -- but this is the
                            //               standard bit to set alongside EM)
    cr0 |=  (1ULL << 5);   // CR0.NE = 1 -- native #MF FPU error reporting,
                            //               not the legacy IRQ13 signaling path
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));

    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);     // CR4.OSFXSR     -- enables FXSAVE/FXRSTOR + SSE
                            //                    (without it, SSE #UD's)
    cr4 |= (1ULL << 10);    // CR4.OSXMMEXCPT -- unmasked SIMD FP errors raise
                            //                    #XM instead of #UD
    __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));

    /* Clean x87 state (control word 0x037F, empty tag word) for THIS core's
     * own starting point. Every real thread's actual FPU/SSE state is
     * governed by its own fpu_state buffer (struct thread, process.h) from
     * its first FXRSTOR onward -- this just establishes a well-defined
     * starting point rather than leaving it to whatever the hardware reset
     * default happens to be. */
    __asm__ volatile ("fninit");
}
