#include "arch/x86_64/irq/ioapic.h"
#include "include/kprintf.h"
#include "acpi/acpi.h"
#include "mm/vmm.h"
#include "drivers/char/serial.h"

#include <stdint.h>


#define IOAPIC_REGSEL         0x00
#define IOAPIC_WIN             0x10

#define IOAPIC_REG_REDTBL_BASE 0x10

static volatile uint8_t *ioapic_base = 0;


static uint32_t ioapic_read(uint32_t reg){
    *(volatile uint32_t *)(ioapic_base + IOAPIC_REGSEL) = reg;
    return *(volatile uint32_t *)(ioapic_base + IOAPIC_WIN);
}

static void ioapic_write(uint32_t reg, uint32_t value){
    *(volatile uint32_t *)(ioapic_base + IOAPIC_REGSEL) = reg;
    *(volatile uint32_t *)(ioapic_base + IOAPIC_WIN) = value;
}

void ioapic_init(void){
    serial_write_string("\n=== IO-APIC init ===\n");

    const struct acpi_info *acpi = acpi_get_info();
    if(!acpi->found || acpi->io_apic_count == 0) {
        kprintf("ACPI not found or no IO-APIC found\n");
        return;
    }
    uint64_t phys = acpi->io_apic_addresses[0];
    ioapic_base = (volatile uint8_t *)vmm_map_mmio(phys, 0x1000);
    if (!ioapic_base) {
        kprintf("Failed to map IO-APIC MMIO\n");
        return;
    }

    // READ VERSION register (index 0x01) - bits 16:31 contain the version number = max redirection entries
    uint32_t version = ioapic_read(0x01);
    kprintf("IO-APIC raw version reg: %x\n", (unsigned int)version);
    uint32_t max_entries = ((version >> 16) & 0xFF) + 1;
    kprintf("IO-APIC version: mapped at %p (phys %p), %u redirection entries\n", (void *)(ioapic_base), (void *)phys, (unsigned int)max_entries);

    // READ ID register (index 0x00) - bits 24:31 contain the APIC ID of the IO-APIC
}

void ioapic_route(uint8_t gsi, uint8_t vector, uint8_t dest_apic_id, bool masked){
    uint32_t low = vector;  // bits 0-7 = vector, rest 0 (fixed, physical, edge, high)
    if (masked) {
        low |= 1 << 16;  // bit 16 = masked
    }
    uint32_t high = (uint32_t)dest_apic_id << 24;  // bits 24-31 = APIC ID of the target processor

    uint32_t reg = IOAPIC_REG_REDTBL_BASE + (gsi * 2);

    // WRITE high first, then low (low has the mask/enable, write it last)
    
    ioapic_write(reg + 1, high);
    ioapic_write(reg, low);

    kprintf("IO-APIC: GSI %u -> vector %u, CPU %u %s\n",
                            (unsigned int)gsi, (unsigned int)vector, (unsigned int)dest_apic_id, masked ? "(masked)" : "(enabled)");


}


// Route a PCI-style interrupt: level-triggered, active-low (bit15 trigger=level,
// bit13 polarity=active-low). The device must clear its own interrupt source in
// its handler so the level de-asserts before EOI, or it re-fires immediately.
void ioapic_route_level(uint8_t gsi, uint8_t vector, uint8_t dest_apic_id, bool active_low){
    uint32_t low = vector | (1u << 15);       // trigger mode = level
    if (active_low) { low |= (1u << 13); }    // polarity = active low
    uint32_t high = (uint32_t)dest_apic_id << 24;
    uint32_t reg = IOAPIC_REG_REDTBL_BASE + (gsi * 2);
    ioapic_write(reg + 1, high);
    ioapic_write(reg, low);
    kprintf("IO-APIC: GSI %u -> vector %u, CPU %u (level, %s)\n",
            (unsigned int)gsi, (unsigned int)vector, (unsigned int)dest_apic_id,
            active_low ? "active-low" : "active-high");
}

void ioapic_mask(uint8_t gsi){
   uint32_t reg = IOAPIC_REG_REDTBL_BASE + (gsi * 2);
   uint32_t low = ioapic_read(reg);
   low |= 1 << 16;  // bit 16 = masked
   ioapic_write(reg, low);
}

void ioapic_unmask(uint8_t gsi){
    uint32_t reg = IOAPIC_REG_REDTBL_BASE + (gsi * 2);
    uint32_t low = ioapic_read(reg);
    low &= ~(1 << 16);  // bit 16 = masked
    ioapic_write(reg, low);
}


