#ifndef _ACPI_H_
#define _ACPI_H_

#include "include/types.h"
#include <stdint.h>


// Common headers on every ACPI table (EXCEPT RSDP which has its own header)
struct acpi_sdt_header {
    char signature[4]; // ASCII signature to identify the table type
    uint32_t length;   // total length of the table, including this header
    uint8_t revision;  // ACPI version (1 for ACPI 1.0, 2 for ACPI 2.0+)
    uint8_t checksum;  // entire table must sum to zero
    char oem_id[6];    // OEM identifier string (padded with spaces)
    char oem_table_id[8]; // OEM table identifier (padded with spaces)
    uint32_t oem_revision; // OEM revision number
    uint32_t creator_id;   // Vendor ID of utility that created the table
    uint32_t creator_revision; // Revision of utility that created the table
} __attribute__((packed));

// Root System Description Pointer (RSDP) structure for ACPI 2.0+
struct rsdp {
    char signature[8]; // "RSD PTR " (with trailing space)
    uint8_t checksum; // entire table must sum to zero
    char oem_id[6];   // OEM identifier string (padded with spaces)
    uint8_t revision; // ACPI version (0 for 1.0, 2 for 2.0+)
    uint32_t rsdt_address; // physical address of the RSDT (ACPI 1.0)

    // Fields below are only valid if revision >= 2
    uint32_t length;   // total length of the RSDP structure (36 bytes for ACPI 2.0+)
    uint64_t xsdt_address; // physical address of the XSDT (ACPI 2.0+)
    uint8_t extended_checksum; // entire table must sum to zero
    uint8_t reserved[3]; // reserved, must be zero
} __attribute__((packed));

// MADT entry header (common to all MADT entry types)
// Signature of MADT is "APIC". Follows the acpi_sdt_header.
struct madt {
    struct acpi_sdt_header header; // standard ACPI table header
    uint32_t local_apic_address;   // physical address of the local APIC
    uint32_t flags;                // bit 0 = PC-AT compatibility, other bits reserved
    // Followed by variable-length array of MADT entries (type, length, data...)
} __attribute__((packed));

// MADT entry header (common to all MADT entry types)
struct madt_entry_header {
    uint8_t type;   // entry type (0=processor, 1=ioapic, 2=interrupt source override, etc.)
    uint8_t length; // total length of this entry, including this header
    // Followed by entry-specific data depending on the type
} __attribute__((packed));


// MADT entry types

#define MADT_TYPE_LOCAL_APIC        0
#define MADT_TYPE_IO_APIC           1
#define MADT_TYPE_INT_OVERRIDE      2
#define MADT_TYPE_NMI_SOURCE        3
#define MADT_TYPE_LOCAL_APIC_NMI    4
#define MADT_TYPE_LOCAL_APIC_OVR    5
#define MADT_TYPE_LOCAL_X2APIC      9

// Type 0: Processor Local APIC
struct madt_local_apic {
    struct madt_entry_header header; // type=0, length=8
    uint8_t acpi_processor_id; // ACPI processor ID (matches _PR._PID)
    uint8_t apic_id;     // Local APIC ID (for programming the APIC)
    uint32_t flags;           // bit 0 = enabled, other bits reserved
} __attribute__((packed));


// Type 1: I/O APIC
struct madt_io_apic {
    struct madt_entry_header header; // type=1, length=12
    uint8_t io_apic_id;   // I/O APIC ID (for programming the APIC)
    uint8_t reserved;     // reserved, must be zero
    uint32_t io_apic_address; // physical address of the I/O APIC's registers
    uint32_t global_system_interrupt_base; // GSI base for this I/O APIC (maps GSI numbers to IRQs)
} __attribute__((packed));


// Type 2: Interrupt Source Override
struct madt_int_override {
    struct madt_entry_header header; // type=2, length=10
    uint8_t bus;   // bus type (0=ISA, 1=PCI, etc.)
    uint8_t source;   // IRQ number on the source bus
    uint32_t global_system_interrupt; // GSI number to route this IRQ to
    uint16_t flags;       // bit 0 = active high, bit 1 = active low, bit 2 = level-triggered, bit 3 = edge-triggered, other bits reserved
} __attribute__((packed));


/* ---- HPET ACPI description table (signature "HPET") ---- */
struct acpi_hpet {
    struct acpi_sdt_header header;      /* "HPET" */
    uint32_t event_timer_block_id;      /* bits[15:8]=vendor, [12]=64-bit, [7:0]=HW rev */
    /* Generic Address Structure for the base */
    uint8_t  gas_addr_space;            /* 0 = memory-mapped */
    uint8_t  gas_bit_width;
    uint8_t  gas_bit_offset;
    uint8_t  gas_access_size;
    uint64_t base_address;              /* physical base of the HPET block */
    uint8_t  hpet_number;
    uint16_t minimum_tick;              /* minimum IRQ period (clock ticks) */
    uint8_t  page_protection;
} __attribute__((packed));

// Max CPUs / IO-APICs we'll
#define ACPI_MAX_CPUS     256
#define ACPI_MAX_IO_APICS 8

// Parsed ACPI data, filled by acpi_init
struct acpi_info{
    bool found; // true if ACPI tables were found and parsed successfully
    uint64_t local_apic_address; // physical address of the local APIC (from MADT)

    uint32_t cpu_count; // number of CPUs found in MADT
    uint8_t cpu_apic_ids[ACPI_MAX_CPUS]; // APIC IDs

    uint32_t io_apic_count; // number of I/O APICs found in MADT
    uint32_t io_apic_addresses[ACPI_MAX_IO_APICS]; // physical addresses of I/O APICs
    uint32_t io_apic_gsi_bases[ACPI_MAX_IO_APICS]; // GSI base numbers for each I/O APIC

    bool     hpet_found;          /* true if HPET table was present and valid */
    uint64_t hpet_address;        /* physical base address of HPET MMIO block */
    uint16_t hpet_minimum_tick;   /* minimum IRQ period in clock ticks */
};


// Discover and parse ACPI tables, fill the acpi_info struct with relevant data for APIC initialization and interrupt routing
const struct acpi_info *acpi_init(void);

// Get the parsed info after acpi_init. Returns NULL if ACPI tables were not found or failed to parse.
const struct acpi_info *acpi_get_info(void);



#endif /* _ACPI_H_ */