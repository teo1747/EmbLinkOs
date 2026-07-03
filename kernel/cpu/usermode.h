#ifndef __USERMODE_H__
#define __USERMODE_H__

// Drop the CPU to ring 3 and run the user_stub payload (write + exit via
// `int 0x80`). Requires syscall_init() to have installed the DPL-3 syscall gate
// first. Does NOT return: the stub's exit(0) lands in sys_exit, which halts.
void enter_user_mode(void);

#endif /* __USERMODE_H__ */
