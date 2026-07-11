#ifndef __IRQ_H__
#define __IRQ_H__

#include <stdint.h>


// Fonction signatures for IRQ handling
typedef void (*irq_handler_t)(void);

// Resiger a handler for IRQ N (0 - 15). Also unmasks the IRQ at the PIC.
void irq_register(uint8_t irq, irq_handler_t handler);

// Unregister handler and mask the IRQ
void irq_unregister(uint8_t irq);

// Called from main.c -installs all 16 IRQ stubs into IDT
void irq_install(void);


#endif /* __IRQ_H__ */