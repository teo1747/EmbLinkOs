#ifndef __IOAPIC_H__
#define __IOAPIC_H__

#include "../include/types.h"
#include <stdint.h>


// Initialize the IO_APIC (map MMIO from ACPI)
void ioapic_init(void);

// Route a GSI (global system interrupt) to a vector on a destination CPU.
// Masked = true leaves the interrupt masked (disabled)
void ioapic_route(uint8_t gsi, uint8_t vector, uint8_t dest_apic_ip, bool masked);

// Route a PCI-style (level-triggered, active-low) interrupt.
void ioapic_route_level(uint8_t gsi, uint8_t vector, uint8_t dest_apic_id, bool active_low);

// Mask/unmask a GSI
void ioapic_mask(uint8_t gsi);
void ioapic_unmask(uint8_t gsi);

#endif /* __IOAPIC_H__ */