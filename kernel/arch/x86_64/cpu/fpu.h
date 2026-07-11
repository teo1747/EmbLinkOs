#ifndef __FPU_H__
#define __FPU_H__

/* Enables FXSAVE/FXRSTOR + SSE for the CALLING core. CR0.EM/MP/NE and
 * CR4.OSFXSR/OSXMMEXCPT are per-core control-register state, not shared --
 * every core that will ever run a thread through kernel_ctx_switch() (the
 * scheduler's context switch, kernel/cpu/kcontext.asm) needs its own call,
 * mirroring vmm_enable_nx_this_cpu()'s per-core contract (kernel/mm/vmm.h).
 * Must run before this core's first kernel_ctx_switch(), since that's the
 * first place an FXSAVE/FXRSTOR executes -- without CR4.OSFXSR set, that
 * (or any other SSE instruction) faults with #UD, not a soft failure. */
void fpu_init_this_cpu(void);

#endif /* __FPU_H__ */
