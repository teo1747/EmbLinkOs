#include "ahci.h"
#include "pci.h"
#include "../mm/vmm.h"
#include "../include/kprintf.h"
#include "serial.h"
#include <stdint.h>


static volatile uint8_t *ahci_abar = 0;
static uint32_t ahci_cap = 0;
static uint32_t ahci_pi = 0;
static struct ahci_port_info ports[AHCI_MAX_PORTS];
static uint32_t port_count = 0;


// 32-bit register access (all AHCI registers are 32-bit)
static inline uint32_t ahci_read(uint32_t offset) {
    return *(volatile uint32_t *)(ahci_abar + offset);
}

static inline void ahci_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(ahci_abar + offset) = value;
}

// Per-port register access
static inline uint32_t ahci_port_read(uint32_t port, uint32_t offset) {
    return ahci_read(0x100 + port * 0x80 + offset);
}

static inline void ahci_port_write(uint32_t port, uint32_t offset, uint32_t value) {
    ahci_write(0x100 + port * 0x80 + offset, value);
}


// Find the AHCI base address in the PCI configuration space
static bool find_ahci_controller(uint8_t *bus, uint8_t *device, uint8_t *function) {
    uint32_t count = pci_devices_count();

    // Iterate over all PCI devices
    for (uint32_t i = 0; i < count; i++) {
        const struct pci_device *d = pci_get_device(i);
        // Class 1 (storage), Subclass 6 (SATA), ProgIF 1 (AHCI) 
        if (d->class_code == 0X01 && d->subclass == 0X06 && d->prog_if == 0X01) {
            *bus = d->bus;
            *device = d->device;
            *function = d->function;
            return true;
        }
    }
    return false;
}

// Describe a port's state in human terms
static const char *det_string(uint8_t det) {
    switch (det) {
        case AHCI_DET_NONE:   return "No device detected";
        case AHCI_DET_PRESENT: return "Device present, not ready";
        case AHCI_DET_READY:  return "Device present and ready";
        default:              return "reserved";
    }
}

static const char *sig_string(uint32_t sig) {
    if (sig == AHCI_SIG_SATA) return "SATA";
    if (sig == AHCI_SIG_SATAPI) return "SATAPI";
    return "unknown";
}

// Initialize the AHCI controller
void ahci_init(void) {
    serial_write_string("\n=== AHCI INIT ===\n");

    // 1. Find the AHCI controller via PCI
    uint8_t bus, device, function;
    if (!find_ahci_controller(&bus, &device, &function)) {
        kprintf("AHCI controller not found\n");
        return;
    }
    kprintf("AHCI: controller at PCI %x:%x.%x\n",
           (unsigned int)bus, (unsigned int)device, (unsigned int)function);
           

    // 2. Enable bus mastering + memory space in PCI command register
    pci_enable_bus_mastering(bus, device, function);
    
    // 3. Get ABAR (BAR5)
    struct pci_bar bar5 = pci_read_bar(bus, device, function, 5);
    if (!bar5.valid || !bar5.is_mmio) {
        kprintf("AHCI: BAR5 invalid or not MMIO\n");
        return;
    }
    kprintf("AHCI: ABAR phys = %p, size = %u\n", (void *)bar5.address, (unsigned int)bar5.size);

    // 4. Map ABAR to virtual memory
    ahci_abar = (volatile uint8_t *)vmm_map_mmio(bar5.address, bar5.size);
    if (!ahci_abar) {
        kprintf("AHCI: failed to map ABAR\n");
        return;
    }
    kprintf("AHCI: ABAR mapped at %p\n", (void *)ahci_abar);

    // 5. Read global capabilities register
    ahci_cap = ahci_read(AHCI_REG_CAP);
    uint32_t ghc = ahci_read(AHCI_REG_GHC);
    ahci_pi = ahci_read(AHCI_REG_PI);
    uint32_t vs = ahci_read(AHCI_REG_VS);

    uint32_t num_ports = (ahci_cap & 0x1F) + 1;      // CAP bits 0-4 + 1
    uint32_t num_slots = ((ahci_cap >> 8) & 0x1F) + 1;   // CAP bits 8-12 + 1
    bool s64a = (ahci_cap & (1U << 31)) != 0;   // 64-bit DMA support addressing
    bool sncq = (ahci_cap & (1U << 30)) != 0;   // NCQ support

    kprintf("AHCI: version %x.%x, CAP=%x, GHC=%x, PI=%x\n",
              (unsigned int)((vs >> 16) & 0xFFFF), (unsigned int)(vs & 0xFFFF), (unsigned int)ahci_cap, (unsigned int)ghc, (unsigned int)ahci_pi);
    
    kprintf("AHCI: %u ports max, %u command slots/port, S64A=%u NCQ=%u\n",
             (unsigned int)num_ports, (unsigned int)num_slots, (unsigned int)s64a, (unsigned int)sncq);
    
    //6. Enable AHCI mode (set GHC.AE)
    ahci_write(AHCI_REG_GHC, ghc | AHCI_GHC_AE);
    ghc = ahci_read(AHCI_REG_GHC);
    kprintf("AHCI: AHCI mode %s (GHC=%x)\n",
            (ghc & AHCI_GHC_AE) ? "enabled" : "Failed to enable", (unsigned int)ghc);
    
    // 7. Enumerate ports
    port_count = 0;
    for (uint32_t i = 0; i < num_ports && i < AHCI_MAX_PORTS; i++) {
        if (!(ahci_pi & (1U << i))) continue; // not Implemented

        uint32_t ssts = ahci_port_read(i, AHCI_PORT_SSTS);
        uint8_t det = ssts & 0x0F;  
        uint32_t sig = ahci_port_read(i, AHCI_PORT_SIG);
        uint32_t tfd = ahci_port_read(i, AHCI_PORT_TFD);

        ports[port_count].port_num = (uint8_t)i;
        ports[port_count].signature = sig;
        ports[port_count].det = det;
        ports[port_count].present = (det == AHCI_DET_READY);
        port_count++;

        kprintf(" Port %u: SSTS=%x (%s), SIG=%x (%s), TFD=%x\n",
                     (unsigned int)i, (unsigned int)ssts, det_string(det), (unsigned int)sig, sig_string(sig), (unsigned int)tfd);

    }
    kprintf("AHCI: %u port(s) implemented\n", (unsigned int)port_count);
}



