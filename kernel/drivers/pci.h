#ifndef __PCI_H__
#define __PCI_H__

#include "../include/types.h"
#include <stdint.h>


// Configuration space registers(port)
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

// Common config space offsets
#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_STATUS       0x06
#define PCI_REVISION     0x08
#define PCI_PROG_IF      0x09
#define PCI_SUBCLASS     0x0A
#define PCI_CLASS        0x0B
#define PCI_HEADER_TYPE  0x0E
#define PCI_BAR0         0x10
#define PCI_INTERRUPT_LINE    0x3C
#define PCI_INTERRUPT_PIN    0x3D


// A discouvered PCI device
 struct pci_device {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
 };

struct pci_bar {
    uint64_t address;   // base physical address (or I/O port base)
    uint64_t size;     // size of the BAR (in bytes)
    bool     is_mmio;   // true if the BAR is memory-mapped false is I/O port-mapped
    bool     is_64bit;  // true if the BAR is 64-bit
    bool     prefetchable; // true if the BAR is prefetchable
    bool     valid;     // true if the BAR is valid (i.e. not zero) false if the BAR is unused(size 0)
};


// Read and size BAR. bar_index is 0-5 for BAR0-BAR5
// For 64 bit BAR, read the pair (index and index + 1)
struct pci_bar pci_read_bar(uint8_t bus, uint8_t device, uint8_t function, uint8_t bar_index);


 #define PCI_MAX_DEVICES 64

 // Read config space of a PCI device
 uint8_t  pci_read8  (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
 uint16_t pci_read16 (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
 uint32_t pci_read32 (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

 // Write config space of a PCI device
 void pci_write32  (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

 // Enumerate all PCI devices, fill internal table, print them
 void pci_init();

 // Access the discovered PCI devices table
 uint32_t pci_devices_count(void);
 const struct pci_device *pci_get_device(uint32_t index);

// Enable bus mastering (command register bit 2) for a device
void pci_enable_bus_mastering(uint8_t bus, uint8_t device, uint8_t function);
#endif /* __PCI_H__ */