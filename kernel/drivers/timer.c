#include "timer.h"
#include "../cpu/irq.h"
#include "../include/kprintf.h"

#include <stdint.h>

static volatile uint64_t ticks = 0;


// IRQ 0 handler called from irq_handler
static void timer_handler(void) {
    ticks++;
    // For testing, print a message every 100 ticks
    if (ticks % 100 == 0) {
        kprintf("[tick %u]\n", (unsigned int)ticks);

    }
}




void timer_init(void) {
    // Register timer_handler for IRQ0 and unmask IRQ0 at PIC
    kprintf("=== Timer init ===\n");
    irq_register(0, timer_handler);
    kprintf("Timer initialized. IRQ0 handler registered.\n");
}



uint64_t timer_get_ticks(void) {
    return ticks;
}