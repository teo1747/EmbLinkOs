#include "ahci.h"
#include "pci.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../include/kprintf.h"
#include "serial.h"
#include <stdint.h>


static volatile uint8_t *ahci_abar = 0;
static uint32_t ahci_cap = 0;
static uint32_t ahci_pi = 0;
static struct ahci_port_info ports[AHCI_MAX_PORTS];
static uint32_t port_count = 0;

// Per-port memory : Command list + received FIS + Command tables. Each port has its own memory area, which is page-aligned (4096 bytes). The command list is 1024 bytes, the received FIS area is 256 bytes, and the command tables are 12288 bytes (32 command tables * 384 bytes each). Total per-port memory = 1024 + 256 + 12288 = 13568 bytes. We round up to the next page boundary (16384 bytes) for alignment.
static struct ahci_port_mem port_mem[AHCI_MAX_PORTS] __attribute__((aligned(4096)));  // Per-port memory (BSS, page-aligned)


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



// Helper functions for AHCI register access


// Wait for a port's command engine to be idle (i.e. wait for PxCMD.CR and PxCMD.FR to be cleared). Returns true if the port is idle, false if it times out after 1 second.
static bool wait_for_port_idle(uint8_t port) {
    for (int i = 0; i < 100000; i++) {
        uint32_t cmd = ahci_port_read(port, AHCI_PORT_CMD);
        if (!(cmd & (1U << 15)) && !(cmd & (1U << 14))) {
            return true; // Port is idle
        }
        
    }
    return false; // Timed out
}

// Stop a port's command engine by clearing PxCMD.ST and waiting for the port to be idle. Returns true if successful, false if it times out.
static bool stop_port(uint8_t port) {
    // Clear ST (Start) bit
    uint32_t cmd = ahci_port_read(port, AHCI_PORT_CMD);
    cmd &= ~((1U << 0) | (1U << 4));   // clear ST and FRE
    ahci_port_write(port, AHCI_PORT_CMD, cmd);
    return wait_for_port_idle(port);
}

// Start a port's command engine by setting PxCMD.ST and waiting for the port to be ready
static void start_port(uint8_t port) {
    // Wait for CR to be cleared (command list processing idle)
    while (ahci_port_read(port, AHCI_PORT_CMD) & (1U << 15)) {
        // Wait
    }
    // Set ST (Start) bit
    uint32_t cmd = ahci_port_read(port, AHCI_PORT_CMD);
    cmd |= (1U << 4); // FRE (FIS receive enable)
    ahci_port_write(port, AHCI_PORT_CMD, cmd); 
    cmd |= (1U << 0); // ST (Start)
    ahci_port_write(port, AHCI_PORT_CMD, cmd);
}


// Initialize a port memory: program the command list base address (PxCLB), the FIS receive area base address (PxFB), and the command table base address (PxCTBA) for each command header. We use the static `port_mem` array for this, which is page-aligned and has enough space for all ports. We also link each command header to its corresponding command table by setting the CTBA in the command header. This sets up the memory structures needed for AHCI command processing.
// of the static buffer, link command headers to their command tables.
static void port_setup_memory(uint8_t port) {
    struct ahci_port_mem *mem = &port_mem[port];

    // Program PxCLB (Command List Base Address)
    // Zero the entire block (BSS is zero-initialized, but we want to be sure)
    uint8_t *ptr = (uint8_t *)mem;
    for (size_t i = 0; i < sizeof(struct ahci_port_mem); i++) {
        ptr[i] = 0;
    }

    // Stop the port before programming memory
    stop_port(port);

    // Program PxCLB (Command List Base Address) AND PxFB (FIS Base Address)
    uint64_t clb_addr = KV2P(&mem->cmd_list[0]);
    uint64_t fb_addr = KV2P(&mem->recv_fis[0]);

    ahci_port_write(port, AHCI_PORT_CLB, (uint32_t)(clb_addr & 0xFFFFFFFF)); // CLB lower 32 bits
    ahci_port_write(port, AHCI_PORT_CLBU, (uint32_t)(clb_addr >> 32)); // CLB upper 32 bits
    ahci_port_write(port, AHCI_PORT_FB, (uint32_t)(fb_addr & 0xFFFFFFFF)); // FB lower 32 bits
    ahci_port_write(port, AHCI_PORT_FBU, (uint32_t)(fb_addr >> 32)); // FB upper 32 bits    

    // For each command header, program the CTBA (Command Table Base Address) to point to the corresponding command table
    for (int i = 0; i < 32; i++) {
        uint64_t ctba_addr = KV2P(&mem->cmd_tables[i]);
        mem->cmd_list[i].ctba = (uint32_t)(ctba_addr & 0xFFFFFFFF); // CTBA lower 32 bits
        mem->cmd_list[i].ctbau = (uint32_t)(ctba_addr >> 32); // CTBA upper 32 bits
    }

    // Clear pending interrupts by writing 1s to PxSERR and PxIS
    ahci_port_write(port, AHCI_PORT_SERR, 0xFFFFFFFF);
    ahci_port_write(port, AHCI_PORT_IS, 0xFFFFFFFF);

    // Start the port
    start_port(port);

}


// Find a free command slot for a port. Returns the slot number (0-31) if a slot is available, or -1 if no slots are free.
static int find_free_command_slot(uint8_t port) {
    uint32_t slots_in_use = ahci_port_read(port, AHCI_PORT_SACT) | ahci_port_read(port, AHCI_PORT_CI);
    for (int i = 0; i < 32; i++) {
        if (!(slots_in_use & (1 << i))) {
            return i;
        }
    }
    return -1;
}

bool ahci_identify_device(uint8_t port_num, void *buffer) {
    // This function will send an IDENTIFY DEVICE command to the specified port and fill the provided buffer with the 512-byte IDENTIFY DEVICE data. It returns true if successful, false otherwise.
    if (port_num >= port_count) {
        return false;
    }

    // 1. Find a free command slot
    int slot = find_free_command_slot(port_num);
    if (slot == -1) {
        kprintf("AHCI: No free command slots on port %u\n", (unsigned int)port_num);
        return false;
    }

    // 2. Set up the command header for the IDENTIFY DEVICE command
    struct ahci_cmd_header *cmd_header = &port_mem[port_num].cmd_list[slot];
    cmd_header->flags = (5 << 0) | (0 << 5); // CFL=5 (20 bytes), write=0 (read), prefetchable=0
    cmd_header->prdtl = 1; // One PRDT entry (we only need one for the 512-byte buffer)
    cmd_header->prdbc = 0; // No bytes transferred yet (this will be updated by the controller)

    // 3. Set up the command table for the IDENTIFY DEVICE command
    struct ahci_cmd_table *cmd_table = &port_mem[port_num].cmd_tables[slot];
    // Zero the command table (especially the CFIS and PRDT entries)
    for (int i = 0; i < sizeof(struct ahci_cmd_table) / sizeof(uint32_t); i++) {
        ((uint32_t *)cmd_table)[i] = 0;
    }

    for (int i = 0; i < 16; i++) {
        cmd_table->acmd[i] = 0;
    }

    cmd_table->prdt[0].dba = 0;
    cmd_table->prdt[0].dbau = 0;
    cmd_table->prdt[0].dbc_i = 0;
    cmd_table->reserved[0] = 0;

    // Fill the CFIS (Command FIS) for the IDENTIFY DEVICE command

    struct fis_reg_h2d *cfis = &cmd_table->cfis;
    cfis->fis_type = AHCI_FIS_TYPE_REG_H2D; // FIS_TYPE_REG_H2D
    cfis->pmport_c = (1 << 7); // Command FIS C bit set to 1 (command)
    cfis->command = 0xEC; // ATA IDENTIFY DEVICE command
    cfis->device = 0; // Device register (0 for master) 
    cfis->count = 0; // Count register (not used for IDENTIFY DEVICE)
    // LBA registers are not used for IDENTIFY DEVICE, so we can leave them as 0

    // Set up the PRDT entry to point to our buffer
    uint64_t buffer_phys = KV2P(buffer);
    if (buffer_phys >> 32) {
        kprintf("AHCI: Buffer above 4GB unsupported here\n");
        return false;
    }
    cmd_table->prdt[0].dba = (uint32_t)(buffer_phys & 0xFFFFFFFF); // Data base address lower 32 bits
    cmd_table->prdt[0].dbau = (uint32_t)(buffer_phys >> 32); // Data base address upper 32 bits
    cmd_table->prdt[0].dbc_i = (512 - 1) | (1U << 31); // Byte count  - 1 (512 bytes) and interrupt on completion

    // 4. Wait for the port to be idle before issuing the command (TFD not BSY/DRQ)
    for (int i = 0; i < 100000; i++) {
        uint32_t tfd = ahci_port_read(port_num, AHCI_PORT_TFD);
        if (!(tfd & (1U << 7)) && !(tfd & (1U << 3))) { // BSY bit 7 =0 and DRQ bit 3 =0
            break;
        }
        if (i == 99999) {
            kprintf("AHCI: Port %u busy, cannot send IDENTIFY DEVICE\n", (unsigned int)port_num);
            return false;
        }
    }

    // 5. Issue the command by setting the corresponding bit in PxCI
    ahci_port_write(port_num, AHCI_PORT_CI, (1U << slot));

    // 6. Wait for the command to complete (check PxCI and PxIS)
    for (int i = 0; i < 100000; i++) {
        uint32_t ci = ahci_port_read(port_num, AHCI_PORT_CI);
        if (!(ci & (1U << slot))) {
            // Command completed
            uint32_t tfd = ahci_port_read(port_num, AHCI_PORT_TFD);
            if (tfd & 0x01) { // Check for Task File Error
                kprintf("AHCI: IDENTIFY error ,TFD=%x\n", (unsigned int)tfd);
                return false;
            }
            return true; // Success
        }
        // also check for errors in PxIS
        uint32_t is = ahci_port_read(port_num, AHCI_PORT_IS);

        if (is & (1U << 30)) { // Check for Task File Error
            kprintf("AHCI: TFE (Task File Error) during IDENTIFY, PxIS=%x\n", (unsigned int)is);
            return false;
        }
    }

    kprintf("AHCI: IDENTIFY command timed out, CI=%x\n", (unsigned int)ahci_port_read(port_num, AHCI_PORT_CI));
    return false; // Timed out
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

    // 8. Set up memory for each port
    for (uint32_t i = 0; i < port_count; i++) {
        if (ports[i].present) {
            port_setup_memory(ports[i].port_num);
            kprintf("AHCI: Port %u memory set up\n", (unsigned int)ports[i].port_num);
        }
    }
}

// Read `count` sectors starting at LBA from an AHCI port into buffer.
// LBA48 READ DMA EXT. Buffer must be kernel-static (KV2P) and contiguous.
bool ahci_read_sectors(uint8_t port_num, uint64_t lba, uint16_t count, void *buffer) {
    if (port_num >= port_count) return false;
    if (count == 0) return false;

    uint32_t bytes = (uint32_t)count * 512;

    int slot = find_free_command_slot(port_num);
    if (slot == -1) {
        kprintf("AHCI: no free slot on port %u\n", (unsigned int)port_num);
        return false;
    }

    // Command header: CFL=5 dwords, read (W=0), 1 PRD
    struct ahci_cmd_header *hdr = &port_mem[port_num].cmd_list[slot];
    hdr->flags = (sizeof(struct fis_reg_h2d) / 4) & 0x1F;
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    // Command table
    struct ahci_cmd_table *tbl = &port_mem[port_num].cmd_tables[slot];
    for (uint32_t i = 0; i < sizeof(struct ahci_cmd_table) / 4; i++) {
        ((uint32_t *)tbl)[i] = 0;
    }

    // H2D FIS: READ DMA EXT with LBA48
    struct fis_reg_h2d *fis = &tbl->cfis;
    fis->fis_type = AHCI_FIS_TYPE_REG_H2D;
    fis->pmport_c = (1 << 7);          // command
    fis->command  = 0x25;              // READ DMA EXT
    fis->device   = (1 << 6);          // LBA mode bit — REQUIRED
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->count = count;

    // PRD: destination buffer
    uint64_t buf_phys = KV2P(buffer);
    if (buf_phys >> 32) {
        kprintf("AHCI: buffer above 4GB unsupported\n");
        return false;
    }
    tbl->prdt[0].dba   = (uint32_t)buf_phys;
    tbl->prdt[0].dbau  = 0;
    tbl->prdt[0].dbc_i = (bytes - 1) | (1U << 31);

    // Wait not-busy, issue, poll completion — same as IDENTIFY
    for (int i = 0; i < 100000; i++) {
        uint32_t tfd = ahci_port_read(port_num, AHCI_PORT_TFD);
        if (!(tfd & 0x88)) break;
        if (i == 99999) { kprintf("AHCI: port busy\n"); return false; }
    }

    ahci_port_write(port_num, AHCI_PORT_CI, (1U << slot));

    for (int i = 0; i < 1000000; i++) {
        if (!(ahci_port_read(port_num, AHCI_PORT_CI) & (1U << slot))) {
            uint32_t tfd = ahci_port_read(port_num, AHCI_PORT_TFD);
            if (tfd & 0x01) {
                kprintf("AHCI: read error, TFD=%x\n", (unsigned int)tfd);
                return false;
            }
            return true;
        }
        if (ahci_port_read(port_num, AHCI_PORT_IS) & (1U << 30)) {
            kprintf("AHCI: TFE during read\n");
            return false;
        }
    }
    kprintf("AHCI: read timeout\n");
    return false;
}

bool ahci_write_sectors(uint8_t port_num, uint64_t lba, uint16_t count, const void *buffer) {
    if (port_num >= port_count) return false;
    if (count == 0) return false;

    uint32_t bytes = (uint32_t)count * 512;

    int slot = find_free_command_slot(port_num);
    if (slot == -1) {
        kprintf("AHCI: no free slot on port %u\n", (unsigned int)port_num);
        return false;
    }

    // Command header: CFL=5 dwords, write (W=1), 1 PRD
    struct ahci_cmd_header *hdr = &port_mem[port_num].cmd_list[slot];
    hdr->flags = ((sizeof(struct fis_reg_h2d) / 4) & 0x1F) | (1 << 5); // W=1 for write
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    // Command table
    struct ahci_cmd_table *tbl = &port_mem[port_num].cmd_tables[slot];
    for (uint32_t i = 0; i < sizeof(struct ahci_cmd_table) / 4; i++) {
        ((uint32_t *)tbl)[i] = 0;
    }

    // H2D FIS: WRITE DMA EXT with LBA48
    struct fis_reg_h2d *fis = &tbl->cfis;
    fis->fis_type = AHCI_FIS_TYPE_REG_H2D;
    fis->pmport_c = (1 << 7);          // command
    fis->command  = 0x35;              // WRITE DMA EXT
    fis->device   = (1 << 6);          // LBA mode bit — REQUIRED
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->count = count;

    // PRD: source buffer
    uint64_t buf_phys = KV2P(buffer);
    if (buf_phys >> 32) {
        kprintf("AHCI: buffer above 4GB unsupported\n");
        return false;
    }
    tbl->prdt[0].dba   = (uint32_t)buf_phys;
    tbl->prdt[0].dbau  = 0;
    tbl->prdt[0].dbc_i = (bytes - 1) | (1U << 31);  

    // Wait not-busy, issue, poll completion — same as IDENTIFY
    for (int i = 0; i < 100000; i++) {
        uint32_t tfd = ahci_port_read(port_num, AHCI_PORT_TFD);
        if (!(tfd & 0x88)) break;
        if (i == 99999) { kprintf("AHCI: port busy\n"); return false; }
    }

    ahci_port_write(port_num, AHCI_PORT_CI, (1U << slot));

    for (int i = 0; i < 1000000; i++) {
        if (!(ahci_port_read(port_num, AHCI_PORT_CI) & (1U << slot))) {
            uint32_t tfd = ahci_port_read(port_num, AHCI_PORT_TFD);
            if (tfd & 0x01) {
                kprintf("AHCI: write error, TFD=%x\n", (unsigned int)tfd);
                return false;
            }
            return true;
        }
        if (ahci_port_read(port_num, AHCI_PORT_IS) & (1U << 30)) {
            kprintf("AHCI: TFE during write\n");
            return false;
        }
    }
    kprintf("AHCI: write timeout\n");
    return false;
}

