#include "acpi.h"
#include "../drivers/serial.h"
#include "../mm/pmm.h"
#include "../include/kprintf.h"
#include <stdint.h>


static struct acpi_info info; // Global variable to hold parsed ACPI info


// Validate checksum of an ACPI table. Returns true if valid, false if invalid.
static bool checksum_ok(const void *ptr, uint64_t length) {
    const uint8_t *bytes = (const uint8_t *)ptr;
    uint8_t sum = 0;
    for (uint64_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

// Compare a 4-character signature to a string.
static bool sig_match(const char *a, const char *b) {
    return (a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]);
}

// Scan a physical memory range for the RSDP signature. Returns pointer to RSDP if found, NULL if not found.
// Returns Virtual pointer to RSDP if found, NULL if not found.
static struct rsdp *scan_for_rsdp(uint64_t phys_start, uint64_t phys_end) {
    const char target[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '}; // "RSD PTR "

    for (uint64_t phys = phys_start; phys < phys_end; phys += 16) {
        char *candidate = (char *)P2V(phys); // Convert physical address to virtual address

        bool match = true;
        for (int i = 0; i < 8; i++) {
            if (candidate[i] != target[i]) {
                match = false;
                break;
            }
        }
        if (!match) {
            continue; // Signature does not match, keep scanning
        }
        // validate the ACPI 1.0 checksum (first 20 bytes)
        struct rsdp *r = (struct rsdp *)candidate;
        if (checksum_ok(r, 20)) {
            return r;
        }
    }
    return NULL;
}


static struct rsdp *find_rsdp() {
    // 1. First try the EBDA (Extended BIOS Data Area) which is often at 0x80000 or 0x90000 0x40E in the BIOS data area contains the segment base of the EBDA. The EBDA is typically 1KB in size, so we scan that range.
    uint16_t ebda_segment = *((uint16_t *)P2V(0x40E)); // EBDA segment is stored at 0x40E in the BIOS data area
    uint64_t ebda_phys = ((uint64_t)ebda_segment) << 4; // Convert segment to physical address
    if (ebda_phys) {
        struct rsdp *r = scan_for_rsdp(ebda_phys, ebda_phys + 1024); // Scan first 1KB of EBDA
        if (r) {
            return r;
        }
    }

    // 2. If not found in EBDA, scan the upper memory area from 0xE0000 to 0xFFFFF
    return scan_for_rsdp(0xE0000, 0x100000);
}   


// Given the RSDP, find a table by its 4-character signature. Returns pointer to the table if found and valid, NULL if not found or invalid.
// If ACPI 2.0+ is supported, uses the XSDT which has 64-bit pointers. If only ACPI 1.0 is supported, uses the RSDT which has 32-bit pointers.
static struct acpi_sdt_header *find_table(const struct rsdp *r, const char *signature) {
    bool use_xsdt = (r->revision >= 2) && (r->xsdt_address != 0);

    if (use_xsdt) {
        struct acpi_sdt_header *xsdt = (struct acpi_sdt_header *)P2V(r->xsdt_address);
        if (!checksum_ok(xsdt, xsdt->length)) {
            serial_write_string("ACPI: XSDT checksum invalid\n");
            return NULL;
        }

        // Number of entries in the XSDT is (length of XSDT - size of header) / 8 (size of each entry)
        uint32_t entries = (xsdt->length - sizeof(struct acpi_sdt_header)) / 8;
        uint64_t *table_ptrs = (uint64_t *)((uint8_t *)xsdt + sizeof(struct acpi_sdt_header));

        for (uint32_t i = 0; i < entries; i++) {
            struct acpi_sdt_header *t = (struct acpi_sdt_header *)P2V(table_ptrs[i]);
            if (checksum_ok(t, t->length) && sig_match(t->signature, signature)) {
                return t;
            }
        }
    } else {
        struct acpi_sdt_header *rsdt = (struct acpi_sdt_header *)P2V(r->rsdt_address);
        if (!checksum_ok(rsdt, rsdt->length)) {
            serial_write_string("ACPI: RSDT checksum invalid\n");
            return NULL;
        }

        // Number of entries in the RSDT is (length of RSDT - size of header) / 4 (size of each entry)
        uint32_t entries = (rsdt->length - sizeof(struct acpi_sdt_header)) / 4;
        uint32_t *table_ptrs = (uint32_t *)((uint8_t *)rsdt + sizeof(struct acpi_sdt_header));

        for (uint32_t i = 0; i < entries; i++) {
            struct acpi_sdt_header *t = (struct acpi_sdt_header *)P2V(table_ptrs[i]);
            if (checksum_ok(t, t->length) && sig_match(t->signature, signature)) {
                return t;
            }
        }
    
    }
    return NULL; // Table not found
}


static void parse_madt(struct madt *madt){
    info.local_apic_address = madt->local_apic_address;

    kprintf("MADT: Local APIC at %p, flages=%x\n",
         (void *)(uint64_t)madt->local_apic_address,(unsigned int)madt->flags);
    
    // Walk the variable-length array of MADT entries to find CPUs and I/O APICs
    // They start right after the madt structure's fixed fields.
    uint8_t *ptr = (uint8_t *)madt + sizeof(struct madt);
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (ptr < end) {
        struct madt_entry_header *eh = (struct madt_entry_header *)ptr;

        if (eh->length == 0) {
            // Malformed - avoid infinite loop
            kprintf("MADT: zero-length entry, stopping\n");
            break;
        }
        switch (eh->type) {
            case MADT_TYPE_LOCAL_APIC: {
                struct madt_local_apic *la = (struct madt_local_apic *)ptr;
                bool enabled = (la->flags & 0x1) != 0;
                 kprintf(" CPU: acpi_id=%u apic_id=%u %s\n",
                    (unsigned int)la->acpi_processor_id, (unsigned int)la->apic_id,
                    enabled ? "enabled" : "disabled");
                if (enabled && info.cpu_count < ACPI_MAX_CPUS) {
                                    
                    info.cpu_apic_ids[info.cpu_count++] = la->apic_id;
                    } 
                break;
            }
            case MADT_TYPE_IO_APIC: {
                struct madt_io_apic *io = (struct madt_io_apic *)ptr;
                    kprintf(" I/O APIC: id=%u addr=%p gsi_base=%u\n",
                        (unsigned int)io->io_apic_id, (void *)(uint64_t)io->io_apic_address,
                        (unsigned int)io->global_system_interrupt_base);
                if (info.io_apic_count < ACPI_MAX_IO_APICS) {
                    info.io_apic_addresses[info.io_apic_count] = io->io_apic_address;
                    info.io_apic_gsi_bases[info.io_apic_count] = io->global_system_interrupt_base;
                    info.io_apic_count++;
                } 
                break;
            }
            case MADT_TYPE_INT_OVERRIDE: {
                struct madt_int_override *ov = (struct madt_int_override *)ptr;
                kprintf(" IRQ Override: src=%u -> gsi=%u flags=%x\n",
                    (unsigned int)ov->source, (unsigned int)ov->global_system_interrupt,
                    (unsigned int)ov->flags);
                break;
            }

            default:
                kprintf(" ( entry type %u, length %u)\n",
                     (unsigned int)eh->type, (unsigned int)eh->length);
                break;
        }
        ptr += eh->length; // Move to the next entry
    }
}


const struct acpi_info *acpi_init(void) {
    serial_write_string("\n=== ACPI init ===\n");

    // Zero the info struct before filling it in
    info.found = false;
    info.local_apic_address = 0;
    info.cpu_count = 0;
    info.io_apic_count = 0;
    info.hpet_found = false;
    info.hpet_address = 0;
    info.hpet_minimum_tick = 0;


    struct rsdp *r = find_rsdp();
    if (!r) {
        kprintf("ACPI: RSDP not found!\n");
        return &info;
    }
    kprintf("ACPI: RSDP found at %p, revision %u\n", (void *)r, (unsigned int)r->revision);
    kprintf("ACPI: %s\n", r->revision >= 2 ? "using XSDT (ACPI 2.0+)" : "using RSDT (ACPI 1.0)");
    

    struct acpi_sdt_header *madt_hdr = find_table(r, "APIC");
    if (!madt_hdr) {
        kprintf("ACPI: MADT not found\n");
        return &info;
    }

    // Validate MADT checksum and length before parsing
    if (!checksum_ok(madt_hdr, madt_hdr->length)) {
        kprintf("ACPI: MADT checksum invalid\n");
        return &info;
    }

    
    parse_madt((struct madt *)madt_hdr);

    /* Look for the HPET table (optional). */
    struct acpi_sdt_header *hpet_hdr = find_table(r, "HPET");
    if (hpet_hdr && checksum_ok(hpet_hdr, hpet_hdr->length) &&
        hpet_hdr->length >= sizeof(struct acpi_hpet)) {
        struct acpi_hpet *ht = (struct acpi_hpet *)hpet_hdr;
        if (ht->base_address && ht->gas_addr_space == 0 /* memory */) {
            info.hpet_found       = true;
            info.hpet_address     = ht->base_address;
            info.hpet_minimum_tick = ht->minimum_tick;
            kprintf("ACPI: HPET at phys %p, minimum_tick=%u\n",
                    (void *)info.hpet_address,
                    (unsigned int)info.hpet_minimum_tick);
        }
    } else {
        kprintf("ACPI: no valid HPET table\n");
    }

    info.found = true;
    kprintf("ACPI: found %u CPU(s), %u IO-APIC(s)\n",
         (unsigned int)info.cpu_count, (unsigned int)info.io_apic_count); 
    return &info;
}


const struct acpi_info *acpi_get_info(void) {
    return &info;
}