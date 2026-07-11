#ifndef __USERMODE_H__
#define __USERMODE_H__

// Launches /init.elf as a real scheduled process (process_create()) and
// blocks (process_wait()) until it exits, then returns. Requires
// syscall_init() to have installed the DPL-3 syscall gate first.
void enter_user_mode(void);

#endif /* __USERMODE_H__ */
