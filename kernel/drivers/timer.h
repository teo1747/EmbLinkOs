#ifndef __TIMER_H__
#define __TIMER_H__

#include <stdint.h>


// Install the timer IRQ Handler (IRQ0)
// PIT is already ruining at BIOS default (~18.2 hz)
void timer_init(void);


// Get the number of ticks since timer init
uint64_t timer_get_ticks(void);

#endif /* __TIMER_H__ */