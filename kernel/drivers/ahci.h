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

// Initialize AHCI controller: Discover, map ABAR, enumerate ports
void ahci_init(void);


#endif /* __AHCI_H__ */