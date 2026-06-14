#ifndef __AHCI_H__
#define __AHCI_H__


#include "../include/types.h"
#include <stdint.h>



#define AHCI_MAX_PORTS      32

// Global Host Control Register offset (relative to ABAR)
#define AHCI_REG_CAP        0x00
#define AHCI_REG_GHC        0x04
#define AHCI_REG_IS         0x08
#define AHCI_REG_PI         0x0C
#define AHCI_REG_VS         0x10

// GHC register bits
#define AHCI_GHC_HRST     (1U << 0)  // Host Software Reset HBA
#define AHCI_GHC_IE       (1U << 1)  // Interrupt Enable
#define AHCI_GHC_AE       (1U << 31)  // AHCI Enable

// Port AHCI Register offset (relative to PORT base = ABAR + 0x100 + N* 0x80)
#define AHCI_PORT_CLB       0x00     
#define AHCI_PORT_CLBU      0x04  
#define AHCI_PORT_FB        0x08  
#define AHCI_PORT_FBU       0x0C  
#define AHCI_PORT_IS        0x10  
#define AHCI_PORT_IE        0x14 
#define AHCI_PORT_CMD       0x18 
#define AHCI_PORT_TFD       0x20
#define AHCI_PORT_SIG       0x24 
#define AHCI_PORT_SSTS      0x28
#define AHCI_PORT_SCTL      0x2C 
#define AHCI_PORT_SERR      0x30 
#define AHCI_PORT_SACT      0x34
#define AHCI_PORT_CI        0x38 

// SSTS det (low nibble): device detection status
#define AHCI_DET_NONE       0x0  // No device detected
#define AHCI_DET_PRESENT    0x1  // Device present, no comm yet
#define AHCI_DET_READY      0x3  // Device present and ready (Phy etablished)

// Port signature values (PxSIG)
#define AHCI_SIG_SATAPI     0xEB140101  // `SATAPI` (optical)
#define AHCI_SIG_SATA       0x00000101  // SATA driver


// A Discovered AHCI Port
struct ahci_port_info {
    uint8_t port_num;  // Port number (0-3)
    uint32_t signature;  // Port signature (PxSIG)
    uint8_t  det;  // Device detection status (SSTS det)
    bool present;  // Port is present
};

// === AHCI Packed structures ===

// Command Header (32 bytes). 32 of these = the command list for a port. Each command header points to a command table.
struct ahci_cmd_header {
    uint16_t flags;      // Command flags (bit 0: cfl, bit 5: write, bit 6: prefetchable, bit 7: reset, bit 8: bist, bit 9: clear busy upon R_OK, bit 10: reserved
    uint16_t prdtl;      // Physical region descriptor table length (number of entries)
    uint32_t prdbc;      // Physical region descriptor byte count transferred
    uint32_t ctba;       // Command table descriptor base address (lower 32 bits)
    uint32_t ctbau;      // Command table descriptor base address (upper 32 bits)
    uint32_t reserved[4]; // Reserved
} __attribute__((packed));

// achi_cmd_header. flags bit layout (low byte = cfl, high byte = flags)
// bit 0-4: cfl (command FIS length in DWORDS, 2-16)
// bit 5: A (ATAPI) = 1, S (SATA) = 0
// bit 6: W (Write: 1 = write to device, 0 = read from device)
// bit 7: P (Prefetchable) = 1, non-prefetchable = 0
// high byte:
// bit 0: R (Reset) = 1, normal = 0)
// bit 1: B (BIST) = 1, normal = 0)
// bit 2: C (Clear busy upon R_OK) = 1)


// FIS types (we only need one type for now, the Register - Host to Device FIS)
#define AHCI_FIS_TYPE_REG_H2D   0x27  // Register - Host to Device FIS
#define AHCI_FIS_TYPE_REG_D2H   0x34  // Register - Device to Host FIS
#define AHCI_FIS_TYPE_DMA_ACT   0x39  // DMA Activate FIS
#define AHCI_FIS_TYPE_DMA_SETUP 0x41  // DMA Setup FIS
#define AHCI_FIS_TYPE_DATA      0x46  // Data FIS
#define AHCI_FIS_TYPE_BIST      0x58  // BIST Activate FIS  
#define AHCI_FIS_TYPE_PIO_SETUP 0x5F  // PIO Setup FIS

// host-to-device FIS (Register - H2D)(20 bytes). the "command packet" sent to the device. The device responds with a Register - D2H FIS.
struct fis_reg_h2d {
    uint8_t fis_type;   // FIS type (FIS_TYPE_REG_H2D = 0x27)
    uint8_t pmport_c;   // bit 7: 1 = command, 0 = control; bits 3:0 = port multiplier (0-15)
    uint8_t command;    // ATA command register
    uint8_t features_lo;   // Feature register, 7:0

    uint8_t lba0;       // LBA low register, 7:0
    uint8_t lba1;       // LBA mid register, 15:8
    uint8_t lba2;       // LBA high register, 23:16
    uint8_t device;     // Device register

    uint8_t lba3;       // LBA register, 31:24
    uint8_t lba4;       // LBA register, 39:32
    uint8_t lba5;       // LBA register, 47:40
    uint8_t features_hi;   // Feature register, 15:8

    uint16_t count;     // Count register (number of sectors to read/write)
    uint8_t icc;        // Isochronous command completion
    uint8_t control;    // Control register

    uint32_t reserved;   // Reserved
} __attribute__((packed));


// Physical Region Descriptor Table Entry (PRDT entry) (16 bytes)
struct ahci_prd {
    uint32_t dba;       // Data base address (lower 32 bits)
    uint32_t dbau;      // Data base address (upper 32 bits)
    uint32_t reserved;  // Reserved
    uint32_t dbc_i;       // Byte count (bit 0-21: byte count, bit 31: interrupt on completion)
} __attribute__((packed));


// Command Table. Holds the CFIS + ATAPI command + PRDT entries. The size of the command table is 256 bytes + (PRDT length * 16 bytes). The command table is pointed to by the command header (CTBA).
// We allocate space for 8 PRDT entries (max 8 * 16 = 128 bytes), so the total size of the command table is 256 + 128 = 384 bytes. The command table must be aligned to a 128-byte boundary.
#define AHCI_PRDS_TABLE_SIZE 8
struct ahci_cmd_table {
    struct fis_reg_h2d cfis;  // Command FIS (host to device)
    uint8_t cfis_pad[64 - sizeof(struct fis_reg_h2d)];      // pad to 64
    uint8_t acmd[16];          // ATAPI command (12 or 16 bytes)
    uint8_t reserved[48];      // Reserved
    struct ahci_prd prdt[AHCI_PRDS_TABLE_SIZE];  // Physical Region Descriptor Table (PRDT) entries
} __attribute__((packed));


// Per-port memory layout (all of this lives in BSS, page-aligned per port)
struct ahci_port_mem {
    struct ahci_cmd_header cmd_list[32];  // Command list (32 command headers, 32 * 32 = 1024 bytes)
    struct ahci_cmd_table cmd_tables[32]; // Command tables (32 command tables, 32 * 384 = 12288 bytes)
    uint8_t recv_fis[256];                   // Received FIS area (256 bytes)
} __attribute__((aligned(4096)));


// send IDENTIFY DEVICE to a port and fill a port and fill a 512-byte buffer with the IDENTIFY DEVICE data. Returns true if successful, false otherwise.
bool ahci_identify_device(uint8_t port_num, void *buffer);

bool ahci_read_sectors(uint8_t port_num, uint64_t lba, uint16_t count, void *buffer);

bool ahci_write_sectors(uint8_t port_num, uint64_t lba, uint16_t count, const void *buffer);
// Initialize AHCI controller: Discover, map ABAR, enumerate ports
void ahci_init(void);


#endif /* __AHCI_H__ */