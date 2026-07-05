#ifndef __LAPIC_H__
#define __LAPIC_H__

#include <stdint.h>
#include "../include/types.h"

// LAPIC register offsets (from local APIC base address)
#define LAPIC_REG_ID            0x020    // Local APIC ID Register (read-only)
#define LAPIC_REG_VERSION       0x030    // Local APIC Version Register (read-only)
#define LAPIC_REG_EOI           0x0B0    // End of Interrupt Register
#define LAPIC_REG_SPURIOUS      0x0F0    // Spurious Interrupt Vector Register
#define LAPIC_REG_LVT_TIMER     0x320    // Local Vector Table entry for Timer
#define LAPIC_REG_TIMER_INIT    0x380  // Local Vector Table entry
#define LAPIC_REG_TIMER_CURRENT 0x390  // Local Vector Table entry
#define LAPIC_REG_TIMER_DIVIDE  0x3E0  // Local Vector Table entry

// Spurious Interrupt Vector Register bits
#define LAPIC_SVR_ENABLE   0x100    // Bit 8: APIC enabled when set, disabled when clear

// LVT Timer bits
#define LAPIC_TIMER_MASKED   0x10000  // Bit 16: Masked when set, unmasked when clear
#define LAPIC_TIMER_PERIODIC 0x20000  // Bit 17: Periodic mode when set, one-shot when clear

// Initialize the local APIC: enable it and set the spurious interrupt vector
void lapic_init(void);

// Send an EOI to the local APIC to signal end of interrupt handling
void lapic_send_eoi(void);

// Get the local APIC ID (for identifying the current CPU)
uint32_t lapic_get_id(void);

// Set up the local APIC timer to generate interrupts at a specified frequency (Hz)
// Calibrate against the PIT to determine the correct initial count value for the desired frequency
void lapic_timer_init(uint8_t vector);

// Ticks elapsed since lapic_timer_init (100 Hz -> 10 ms per tick)
uint64_t lapic_timer_get_ticks(void);

#endif /* __LAPIC_H__ */