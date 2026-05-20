#include "timer.h"
#include "../cpu/irq.h"
#include "../include/kprintf.h"
#include "../mm/kheap.h"


#include <stdint.h>

static volatile uint64_t ticks = 0;
volatile int heap_stress_enable = 0;


// IRQ 0 handler called from irq_handler

static void timer_handler(void) {
    ticks++;

    if (heap_stress_enable) {
        void *p = kmalloc(48);
        if (p) {
            *(volatile uint64_t *)p = 0xBB;
            kfree(p);
        }
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