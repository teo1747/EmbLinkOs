#ifndef __PIT_H__
#define __PIT_H__

#include <stdint.h>

// Busywait delay for approximately the given number of milliseconds using the PIT channel 2. Not very accurate, but simple and doesn't require interrupts or LAPIC timer.    
void pit_delay_ms(uint32_t ms);
#endif /* __PIT_H__ */