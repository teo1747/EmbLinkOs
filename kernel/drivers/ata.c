#include "ata.h"
#include "pci.h"
#include "../mm/pmm.h"
#include "../include/io.h"
#include "../include/kprintf.h"
#include "../drivers/serial.h"
#include "../cpu/irq.h"
#include "../cpu/ioapic.h"
#include "../include/errno.h"


#include <stdint.h>

#define ATA_PRIMARY_IRQ  14
#define ATA_PRIMARY_VECTOR 46   // IDT vector for IRQ14 (32 + 14)

// Bus Master IDE register offsets (from BAR4 base, Primary channel)
#define BMIDE_COMMAND      0x00
#define BMIDE_STATUS       0x02
#define BMIDE_PRDT         0x04

// BMIDE_COMMAND bits
#define BMIDE_CMD_START    0x01     // Start/Stop bus master transfer
#define BMIDE_CMD_READ     0x08     // Direction: 1 = device to memory, 0 = memory to device

// BMIDE_STATUS bits
#define BMIDE_STATUS_ACTIVE    0x01     // Busy
#define BMIDE_STATUS_ERROR     0x02     // Error
#define BMIDE_STATUS_IRQ       0x04     // Interrupt request (set when transfer completes)

// ATA DMA commands
#define ATA_CMD_IDENTIFY_DMA 0xA1
#define ATA_CMD_READ_DMA     0xC8   
#define ATA_CMD_WRITE_DMA    0xCA

// Physical address of the PRDT (Physical Region Descriptor Table) must be 4K-aligned and in the first 4GB of memory (for 32-bit addressing)
struct prd{
    uint32_t phys_addr;   // Physical address of the data buffer (must be 4K-aligned)
    uint16_t byte_count;  // Byte count for this entry (max 64KB, but we will use 4K pages)
    uint16_t flags;       // Flags: bit 15 = end of table
} __attribute__((packed));

#define PRD_EOT 0x8000;


//Block device structs for each detected ATA drive. The block layer will use these to read/write sectors from/to
static struct embk_block_device ata_block_devices[ATA_MAX_DRIVES];


// A single PRD, 4-bytes aligned. won't cross 4K boundary, (it's 8 bytes in BSS)
// Static so it has a fixed physical address we can compute with KV2P.
static struct prd dma_prdt[1] __attribute__((aligned(4)));

// Bus master I/O base (BAR4 of the IDE controller)
static uint16_t bmide_base;

static volatile bool ata_irq_fired = false;

static struct ata_drive drives[ATA_MAX_DRIVES];
static uint32_t drive_count = 0;


static void ata_irq_handler(void) {
    // Reading the status register acknowledge the interrupt at the drive.
    // Without this, the controller never sends another interrupt.
    inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    ata_irq_fired = true;
    // LAPIC EOI (End Of Interrupt) is sent by the common irq_handler() after return.
}

static int ata_wait_irq(void){
    // Sleep until the ATA IRQ sets the flag. the 100 hz timer guarantees
    // We wake periodically to re-check even if we miss the exact moment.
    int timeout = 0;
    while (!ata_irq_fired) {
        __asm__ volatile ("hlt");
        if (++timeout > 1000000) {
        kprintf("ATA IRQ timeout\n");
        return -1; // safety: never hang forever on bad hardware
        }
    }
    ata_irq_fired = false;
    return 0;
}


// 400ns delY: READ ALTERNATE STATUS 4 TIMESS (~100ns each)

static void ata_io_wait(uint16_t ctrl_base) {
    inb(ctrl_base);
    inb(ctrl_base);
    inb(ctrl_base); // 400ns delay
    inb(ctrl_base);
}


// SPIN until BSY clears. Returns 0 on success, -1 on error.
static int ata_wait_not_busy(uint16_t io_base) { 
    // Generous timeout to aviod hanging forever on bad hardware
    for (int i = 0; i < 100000; i++) { 
        if (!(inb(io_base + ATA_REG_STATUS) & ATA_STATUS_BSY)) { 
            return 0; 
        }
        
    }
    return -1;
}


// Spin until DRQ sets (data ready). Returns 0 on success, -1 on error.
static int ata_wait_drq(uint16_t io_base) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        if (status & ATA_STATUS_ERR) {
            return -1;
        }
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ)) {
            return 0;
        }
  
    }
  return -1;
}


// Try to detect one drive. Return true if found, false if not.
static bool ata_detect(uint16_t io_base, uint16_t ctrl_base, bool is_slave, struct ata_drive *out) {
    // Select drive: 0xA0 slave (non-LBA select for IDENTIFY)
    outb(io_base + ATA_REG_DRIVE, is_slave ? 0xB0 : 0xA0);
    ata_io_wait(ctrl_base);

    // Zero out the LBA/count registers, then send Identify command
    outb(io_base + ATA_REG_SECCOUNT, 0);
    outb(io_base + ATA_REG_LBA_LOW, 0); 
    outb(io_base + ATA_REG_LBA_MID, 0); 
    outb(io_base + ATA_REG_LBA_HIGH, 0); 
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(io_base + ATA_REG_STATUS);
    if (status == 0) {
        return false;
    }

    if (ata_wait_not_busy(io_base) < 0) {
        return false;
    }

    // ATAPI/SATA signature check
    if (inb(io_base + ATA_REG_LBA_MID) != 0 || inb(io_base + ATA_REG_LBA_HIGH) != 0) {
        return false; // Not a plain ATA drive
    }

    if (ata_wait_drq(io_base) < 0){
        return false;
    }

    // Read the Identify data 256-word
    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(io_base + ATA_REG_DATA);
    }

    // Total sectors: LBA28 (words 60-61), fall back to CHS geometry
    uint32_t total = identify_data[60] | ((uint32_t)identify_data[61] << 16);
    if (total == 0) {
        total = (uint32_t)identify_data[1] * identify_data[3] * identify_data[6];   // cylinders * heads * sectors-per-track
    }

    // Model string: words 27 - 46, byte-swapped
    for (int i = 0; i < 20 ; i++) {
        uint16_t w = identify_data[27 + i];
        out->model[2*i]     = (char) (w >> 8); // High byte first
        out->model[2*i + 1] = (char) (w & 0xFF); 
    }
    out->model[40] = '\0'; // Null-terminate the string

    out->present = true;
    out->io_base = io_base;
    out->ctrl_base = ctrl_base;
    out->is_slave = is_slave;
    out->total_sectors = total;

    return true;

}



uint32_t ata_drive_count(void) {
    return drive_count;
}


const struct ata_drive *ata_get_drive(uint32_t index) {
    if (index >= drive_count) return NULL;
    return &drives[index];
}


// Select drive and program LBA28 on given drive. drive 0 = master, 1 = slave
static void ata_setup_lba(const struct ata_drive *d, uint64_t lba, uint8_t count) {
    uint8_t drive_sel = (d->is_slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F);

    outb(d->io_base + ATA_REG_DRIVE, drive_sel);
    ata_io_wait(d->ctrl_base);

    outb(d->io_base + ATA_REG_SECCOUNT, count);
    outb(d->io_base + ATA_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(d->io_base + ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(d->io_base + ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
}


int ata_read_sectors(uint32_t drive_index, uint64_t lba, uint8_t count, void *buffer) {
    if (drive_index >= drive_count) return -1;
    if (count == 0) return -1;

    const struct ata_drive *d = &drives[drive_index];
    uint16_t *buffer16 = (uint16_t *)buffer; // Cast buffer to uint16_t pointer

    if (ata_wait_not_busy(d->io_base) < 0) return -1;

    ata_irq_fired = false; // Clear the flag for the first sector's IRQ
    ata_setup_lba(d, lba, count); // Setup LBA and count
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO); // Send Read command

    // Read each sector
    for (int i = 0; i < count; i++) {
        if (ata_wait_irq() < 0){
            kprintf("ATA: IRQ wait timeout at sctor %u\n", (unsigned int)i);
            return -1; // Wait for DRQ to set;
        }
        for (int j = 0; j < 256; j++) {
            buffer16[j] = inw(d->io_base + ATA_REG_DATA); // Read 256 bytes
        }
        buffer16 += 256; // Move buffer pointer forward
        // Clear the flag for the next sector's IRQ
        ata_irq_fired = false;
    }   
    return 0;
}

int ata_write_sectors(uint32_t drive_index, uint64_t lba, uint8_t count, const void *buffer) {
    if (drive_index >= drive_count) return -1;
    if (count == 0) return -1;

    const struct ata_drive *d = &drives[drive_index]; // Get drive pointer
    const uint16_t *buffer16 = (const uint16_t *)buffer; // Cast buffer to uint16_t pointer

    if (ata_wait_not_busy(d->io_base) < 0) return -1;

    ata_irq_fired = false;
    ata_setup_lba(d, lba, count);
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO); // Send Write command

    // Write each sector
    for (int i = 0; i < count; i++) {
        // Wait for the drive to be ready to ACCEPT this sector's data
        // After WRITE command (and after each completed section), the drive sets DRQ when its buffer is ready
        // We poll DRQ here - the drive doesn't raise an irq to ask for data, only to confirm completion. 
        if (ata_wait_drq(d->io_base) < 0) {
            kprintf("ATA: write to DRQ failed at sector %u\n", (unsigned int)i); // Wait for DRQ to set
            return -1; // Wait for DRQ to set
        }

        // Write 256 bytes to the drive
        ata_irq_fired = false;
        for (int j = 0; j < 256; j++) {
            outw(d->io_base + ATA_REG_DATA, buffer16[j]); // Write 256 bytes
        }
        buffer16 += 256; // Move buffer pointer forward

        // Now that the drive write to the platter and raises IRQ 14 WHEN done.
        // Wait for the completion IRQ
        if (ata_wait_irq() < 0) {
            kprintf("ATA: IRQ wait timeout at sector %u\n", (unsigned int)i);
            return -1; // Wait for DRQ to set
        }
    }

    // Flush the cache after writing
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH); // Send Cache flush command
    ata_wait_not_busy(d->io_base); // Wait for BSY to clear

    return 0;
}

int ata_read_dma(uint32_t drive_index, uint64_t lba, uint8_t count, void *buffer) {
    if (drive_index >= drive_count) return -1; 
    if (count == 0) return -1;
    if (bmide_base == 0) return -1; // DMA not available without valid bus master I/O base

    const struct ata_drive *d = &drives[drive_index]; // Get drive pointer

    // Transfer size in bytes. Single PRD must not cross 64KB boundary, so we limit to 64KB (128 sectors) per transfer. For simplicity, we also require the buffer to be physically contiguous and not cross 4K boundary.
    uint32_t byte_count = (uint32_t)count * ATA_SECTOR_SIZE;
    if (byte_count > 64 * 1024) {
        kprintf("ATA: DMA transfer too large (max 128 sectors)\n");
        return -1;
    }

    // Physical address of the destination buffer.
    // NOTE: buffer must be a kernel static/stack address (KV2P applies)
    // and physically contiguous, not crossing a 64KB boundary.
    uint64_t buf_phys = KV2P(buffer);
    if ((buf_phys >> 32) != 0) {
        kprintf("ATA DMA: buffer above 4GB, unsupported\n");
        return -1;
    }

    // 1. Built the PRDT (single entry since we limit to 64KB)
    dma_prdt[0].phys_addr = (uint32_t)buf_phys;
    dma_prdt[0].byte_count = (uint16_t)byte_count;  // 0 would mean 64KB; bytes<=64kb ok
    dma_prdt[0].flags = PRD_EOT; // End of table

    uint64_t prdt_phys = KV2P(dma_prdt); // Physical address of the PRDT

    // 2. Stop any previous DMA, then program the PRDT physical address and start the transfer
    outb(bmide_base + BMIDE_COMMAND, 0); // Stop any previous DMA
    outl(bmide_base + BMIDE_PRDT, (uint32_t)prdt_phys); // Program PRDT physical address

    // 3. Set direction = read (controller writes into Ram)
    outb(bmide_base + BMIDE_COMMAND, BMIDE_CMD_READ); // Set direction = read

    // 4. Clear interrupt + error status bits by writing 1s 
    uint8_t status = inb(bmide_base + BMIDE_STATUS);
    outb(bmide_base + BMIDE_STATUS, status | BMIDE_STATUS_IRQ | BMIDE_STATUS_ERROR); // Clear status bits

    // 5. Program the ATA registers (drive, LBA, count) - same as PIO
    if (ata_wait_not_busy(d->io_base) < 0) return -1;
    ata_setup_lba(d, lba, count);

    // 6. Clear IRQ fired flag, issue READ DMA command to the drive (different from PIO command)
    ata_irq_fired = false;
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_READ_DMA); // Send READ DMA command

    // 7. Start the bus master engine (set start bit, keep read direction)
    outb(bmide_base + BMIDE_COMMAND, BMIDE_CMD_READ | BMIDE_CMD_START); // Start the transfer

    // 8. Wait for completion IRQ, check for errors
    if (ata_wait_irq() < 0) {
        kprintf("ATA: IRQ wait timeout\n");
        outb(bmide_base + BMIDE_COMMAND, 0); // Stop the bus master
        return -1; // Wait for DRQ to set
    }

    // 9. Stop the bus master engine
    outb(bmide_base + BMIDE_COMMAND, 0); // Stop the transfer

    // 10. Check for errors
    uint8_t st = inb(bmide_base + BMIDE_STATUS);
    outb(bmide_base + BMIDE_STATUS, st | BMIDE_STATUS_IRQ | BMIDE_STATUS_ERROR); // Clear status bits
    if (st & BMIDE_STATUS_ERROR) {
        kprintf("ATA DMA read error: status %x\n", (unsigned int)st);
        return -1;
    }

    // Data is now in the buffer, placed there by the controller via DMA. We can return success.
    return 0;
}


int ata_write_dma(uint32_t drive_index, uint64_t lba, uint8_t count, const void *buffer) {
    if (drive_index >= drive_count) return -1;
    if (count == 0) return -1;
    if (bmide_base == 0) return -1;

    const struct ata_drive *d = &drives[drive_index];

    uint32_t bytes = (uint32_t)count * ATA_SECTOR_SIZE;
    if (bytes > 64 * 1024) {
        kprintf("ATA DMA: write too large for single PRD\n");
        return -1;
    }

    uint64_t buf_phys = KV2P(buffer);
    if ((buf_phys >> 32) != 0) {
        kprintf("ATA DMA: buffer above 4GB, unsupported\n");
        return -1;
    }

    // 1. Build PRDT (single entry pointing at the source buffer)
    dma_prdt[0].phys_addr  = (uint32_t)buf_phys;
    dma_prdt[0].byte_count = (uint16_t)bytes;
    dma_prdt[0].flags      = PRD_EOT;

    uint64_t prdt_phys = KV2P(dma_prdt);

    // 2. Stop, program PRDT address
    outb(bmide_base + BMIDE_COMMAND, 0);
    outl(bmide_base + BMIDE_PRDT, (uint32_t)prdt_phys);

    // 3. Set direction = WRITE (controller reads RAM). Direction bit CLEARED.
    outb(bmide_base + BMIDE_COMMAND, 0);   // direction bit 3 = 0 means RAM->disk

    // 4. Clear interrupt + error status
    uint8_t st = inb(bmide_base + BMIDE_STATUS);
    outb(bmide_base + BMIDE_STATUS, st | BMIDE_STATUS_IRQ | BMIDE_STATUS_ERROR);

    // 5. Program ATA registers
    if (ata_wait_not_busy(d->io_base) < 0) return -1;
    ata_setup_lba(d, lba, count);

    // 6. Clear IRQ flag, issue WRITE DMA command
    ata_irq_fired = false;
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_DMA);

    // 7. Start the bus master (start bit set, direction bit 0 for write)
    outb(bmide_base + BMIDE_COMMAND, BMIDE_CMD_START);

    // 8. Wait for completion IRQ
    if (ata_wait_irq() < 0) {
        kprintf("ATA DMA: write IRQ timeout\n");
        outb(bmide_base + BMIDE_COMMAND, 0);
        return -1;
    }

    // 9. Stop bus master
    outb(bmide_base + BMIDE_COMMAND, 0);

    // 10. Check error, clear status
    uint8_t status = inb(bmide_base + BMIDE_STATUS);
    outb(bmide_base + BMIDE_STATUS, status | BMIDE_STATUS_IRQ | BMIDE_STATUS_ERROR);
    if (status & BMIDE_STATUS_ERROR) {
        kprintf("ATA DMA: write error (status %x)\n", (unsigned int)status);
        return -1;
    }

    // 11. Flush the drive cache so data is durable on the platter
    ata_irq_fired = false;
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (ata_wait_irq() < 0) {
        ata_wait_not_busy(d->io_base);   // fallback if no flush IRQ
    }

    return 0;
}

// Block-layer adapter: read. Pulls the ATA drive index from driver_data,
// dispatches to the DMA READ PATH.
static int ata_block_read(struct embk_block_device *dev, uint64_t lba, uint32_t count, void *buffer) {
    if (!dev || !buffer) return -EMBK_EINVAL;
    uint32_t drive_index = (uint32_t)(uintptr_t)dev->driver_data; // driver_data holds the ATA drive index
    
    // ata_read_dma takes uint8_t count, chuck if needed (count <= 128 for one PRD)
    uint8_t *ptr = (uint8_t *)buffer;
    while (count > 0) {
        uint8_t chunk = (count > 64) ? 64 : (uint8_t)count; // 64 sectors = 32KB, safe for one PRD
        if (ata_read_dma(drive_index, lba, chunk, ptr) != 0) {
            return -EMBK_EIO; // I/O error
        }
        lba += chunk;
        ptr += (uint32_t)chunk * ATA_SECTOR_SIZE;
        count -= chunk;
    }
    return EMBK_OK;
}


static int ata_block_write(struct embk_block_device *dev, uint64_t lba, uint32_t count, const void *buffer) {
    if (!dev || !buffer) return -EMBK_EINVAL;
    uint32_t drive_index = (uint32_t)(uintptr_t)dev->driver_data; // driver_data holds the ATA drive index

    const uint8_t *ptr = (const uint8_t *)buffer;
    while (count > 0) {
        uint8_t chunk = (count > 64) ? 64 : (uint8_t)count; // 64 sectors = 32KB, safe for one PRD
        if (ata_write_dma(drive_index, lba, chunk, ptr) != 0) {
            return -EMBK_EIO; // I/O error
        }
        lba += chunk;
        ptr += (uint32_t)chunk * ATA_SECTOR_SIZE;
        count -= chunk;
    }
    return EMBK_OK;
}


void ata_register_block_devices(void) {
    for (uint32_t i = 0; i < drive_count; i++) {
        ata_block_devices[i].block_count = drives[i].total_sectors;
        ata_block_devices[i].block_size = ATA_SECTOR_SIZE;
        ata_block_devices[i].read = ata_block_read;
        ata_block_devices[i].write = ata_block_write;
        ata_block_devices[i].driver_data = (void *)(uintptr_t)i; // Store the drive index
        embk_block_register(&ata_block_devices[i]);
    }
}


void ata_init(void) {
    serial_write_string("\n=== ATA init ===\n");
    drive_count = 0;

    struct { uint16_t io, ctrl; bool slave; } candidates[4] = {
        { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, false },
        { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, true },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, false },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, true }
    };

    for (int i = 0; i < 4; i++) {
        struct ata_drive d;
        if (ata_detect(candidates[i].io, candidates[i].ctrl, candidates[i].slave, &d)) {
        if (d.present) {
            drives[drive_count] = d;
            }
        kprintf("ATA[%u]: %s %s, %u sectors (%u KB) - %s\n",
                (unsigned int)drive_count, candidates[i].io == ATA_PRIMARY_IO ? "primary" : "secondary",
                candidates[i].slave ? "slave" : "master",
                (unsigned int)d.total_sectors, (unsigned int)(d.total_sectors / 2), d.model);
        drive_count++;
        }

    }

    // IDE conntroller is at PCI bus 0, device 1, function 1. Read its BAR4 for bus master registers
    pci_enable_bus_mastering(0, 1, 1); // Enable bus mastering for the IDE controller

    // Read BAR4 for bus master registers
    struct pci_bar bar4 = pci_read_bar(0, 1, 1, 4);
    if (bar4.valid && !bar4.is_mmio) {
        bmide_base = (uint16_t)bar4.address;
        kprintf("ATA: bus-master base (BAR4) = %X\n",
                 (unsigned int)bmide_base);

    } else {
        kprintf("ATA: invalid BAR4 for bus master registers\n");
        bmide_base = 0; // fallback to 0, but DMA won't work
    }
    kprintf("ATA: Bus Master IDE I/O base at 0x%X\n", (unsigned int)bmide_base);

    if (drive_count == 0) {
        kprintf("ATA: no drives detected.\n");   
    } else {
        kprintf("ATA: %u drive(s) detected.\n", (unsigned int)drive_count);
    }
    // Install IRQ handler, routing to IRQ14 via IOAPIC
    irq_register(ATA_PRIMARY_IRQ, ata_irq_handler);
    ioapic_route(ATA_PRIMARY_IRQ, ATA_PRIMARY_VECTOR, 0, false); // Route to CPU 0, unmasked
    kprintf("ATA: IRQ14 routed to vector %u\n", (unsigned int)ATA_PRIMARY_VECTOR);

    // Register block devices for each detected drive
    ata_register_block_devices();
}

