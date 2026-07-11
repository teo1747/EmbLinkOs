#include "drivers/usb/xhci.h"
#include "drivers/input/keyboard.h"

#include "include/kprintf.h"
#include "include/kstring.h"   // memcpy
#include "include/errno.h"     // EMBK_* error codes
#include "mm/vmm.h"
#include "mm/pmm.h"   // KV2P: kernel-image virtual -> physical for DMA
#include "block/block.h"       // block-device registration for USB mass storage
#include "arch/x86_64/irq/irq.h"           // irq_register (interrupt-driven event servicing)
#include "arch/x86_64/irq/ioapic.h"        // ioapic_route_level for the PCI interrupt

#include <stdint.h>

#define XHCI_CAPLENGTH   0x00
#define XHCI_HCIVERSION  0x02
#define XHCI_HCSPARAMS1  0x04
#define XHCI_HCCPARAMS1  0x10
#define XHCI_DBOFF       0x14
#define XHCI_RTSOFF      0x18

#define XHCI_OP_USBCMD   0x00
#define XHCI_OP_USBSTS   0x04
#define XHCI_OP_PAGESIZE 0x08
#define XHCI_OP_CRCR     0x18
#define XHCI_OP_DCBAAP   0x30
#define XHCI_OP_CONFIG   0x38

#define XHCI_RT_IR0      0x20
#define XHCI_IR_IMAN     0x00
#define XHCI_IR_ERSTSZ   0x08
#define XHCI_IR_ERSTBA   0x10
#define XHCI_IR_ERDP     0x18

#define XHCI_PORTSC_BASE 0x400
#define XHCI_PORT_STRIDE 0x10

#define XHCI_USBCMD_RUN_STOP (1U << 0)
#define XHCI_USBCMD_HCRST    (1U << 1)
#define XHCI_USBCMD_INTE     (1U << 2)

#define XHCI_USBSTS_HCH      (1U << 0)
#define XHCI_USBSTS_EINT     (1U << 3)   // Event Interrupt (write-1-to-clear)
#define XHCI_USBSTS_CNR      (1U << 11)

#define XHCI_IMAN_IP          (1U << 0)  // Interrupt Pending (write-1-to-clear)
#define XHCI_IMAN_IE          (1U << 1)  // Interrupt Enable

#define XHCI_PORTSC_CCS      (1U << 0)
#define XHCI_PORTSC_PED      (1U << 1)
#define XHCI_PORTSC_PR       (1U << 4)
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_SPEED_MASK  (0xFU << XHCI_PORTSC_SPEED_SHIFT)
#define XHCI_PORTSC_RW1C_MASK   0x00FE0000U

#define XHCI_TRB_TYPE_SHIFT 10
#define XHCI_TRB_LINK            6U
#define XHCI_TRB_ENABLE_SLOT     9U
#define XHCI_TRB_ADDRESS_DEVICE 11U
#define XHCI_TRB_CFG_ENDPOINT   12U  // Configure Endpoint command TRB type
#define XHCI_TRB_CMD_COMPLETION 33U

#define XHCI_TRB_CYCLE_BIT  (1U << 0)
#define XHCI_TRB_TC_BIT     (1U << 1)

#define XHCI_CMD_RING_TRBS   256U
#define XHCI_EVENT_RING_TRBS 256U
#define XHCI_EP0_RING_TRBS    32U
#define XHCI_INT_IN_RING_TRBS 32U  // interrupt-IN transfer ring depth
// Number of distinct receive buffers kept armed on an interrupt-IN endpoint. The
// ring must stay populated or the endpoint stalls after one report, and each
// outstanding TRB needs its OWN buffer so a later report can't overwrite an
// earlier one before software reads it. Must be < XHCI_INT_IN_RING_TRBS - 1.
#define XHCI_INTIN_NUM_BUFS   8U

// ---- USB Mass Storage (Bulk-Only Transport + SCSI) ----
#define XHCI_BULK_RING_TRBS   16U   // per bulk-endpoint transfer ring depth
#define MSC_XFER_SECTORS      8U    // sectors moved per BOT data phase
#define MSC_SECTOR_SIZE       512U
#define MSC_XFER_BYTES        (MSC_XFER_SECTORS * MSC_SECTOR_SIZE) // 4 KB bounce
// xHCI endpoint types (EP Context, dword1 bits[5:3]).
#define XHCI_EP_TYPE_BULK_OUT 2U
#define XHCI_EP_TYPE_BULK_IN  6U
// Bulk-Only Transport signatures.
#define MSC_CBW_SIGNATURE 0x43425355U // 'USBC'
#define MSC_CSW_SIGNATURE 0x53425355U // 'USBS'
// SCSI opcodes used here.
#define SCSI_TEST_UNIT_READY 0x00U
#define SCSI_REQUEST_SENSE   0x03U
#define SCSI_INQUIRY         0x12U
#define SCSI_READ_CAPACITY10 0x25U
#define SCSI_READ10          0x28U
#define SCSI_WRITE10         0x2AU

// The per-controller runtime bundle below is large: each tracked slot carries a
// 1 KB input context, a 1 KB device context, and several DMA rings. The original
// sizing (USB_MAX_CONTROLLERS = 16 controllers × 32 slots) produced ~1.8 MB of
// BSS, which pushed kernel_end into the reserved boot-stack region and let the
// downward-growing stack corrupt these globals — the boot hang. Bound the heavy
// runtime table separately and realistically: a machine has one or two xHCI
// controllers, and this early bring-up path only addresses a few devices.
#define XHCI_MAX_SLOTS_TRACKED 8U
#define XHCI_MAX_CONTROLLERS   2U

struct xhci_trb {
    uint32_t d0;
    uint32_t d1;
    uint32_t d2;
    uint32_t d3;
} __attribute__((packed));

struct xhci_erst_entry {
    uint64_t ring_segment_base;
    uint32_t ring_segment_size;
    uint32_t reserved;
} __attribute__((packed));

// This runtime bundle is intentionally static/aligned so the controller can DMA
// into command/event structures from very early boot without allocator coupling.
struct xhci_runtime_state {
    bool used;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t cmd_cycle;
    uint8_t evt_cycle;
    uint16_t cmd_enqueue;
    uint16_t evt_dequeue;
    uint8_t context_size;
    uint8_t tracked_slots;

    uint8_t slot_to_port[XHCI_MAX_SLOTS_TRACKED];
    uint8_t slot_speed[XHCI_MAX_SLOTS_TRACKED];
    uint8_t slot_active[XHCI_MAX_SLOTS_TRACKED];
    // Per-slot EP0 transfer ring enqueue pointer and cycle bit.
    uint16_t ep0_enqueue[XHCI_MAX_SLOTS_TRACKED];
    uint8_t  ep0_cycle[XHCI_MAX_SLOTS_TRACKED];
    // Per-slot interrupt-IN ring state (used after Configure Endpoint).
    uint16_t intin_enqueue[XHCI_MAX_SLOTS_TRACKED];
    uint8_t  intin_cycle[XHCI_MAX_SLOTS_TRACKED];
    uint8_t  intin_ep_addr[XHCI_MAX_SLOTS_TRACKED]; // EP address from descriptor
    uint8_t  intin_interval[XHCI_MAX_SLOTS_TRACKED]; // bInterval from descriptor
    uint16_t intin_mps[XHCI_MAX_SLOTS_TRACKED];      // max packet size (report length)
    bool     intin_active[XHCI_MAX_SLOTS_TRACKED];   // true after Configure Endpoint
    uint8_t  prev_keys[XHCI_MAX_SLOTS_TRACKED][6];   // last report's keycodes (edge detect)
    // Distinct receive buffers cycled across the interrupt-IN ring. head = next
    // buffer to arm, tail = next completed buffer to read (both mod NUM_BUFS).
    uint8_t  intin_buf_head[XHCI_MAX_SLOTS_TRACKED];
    uint8_t  intin_buf_tail[XHCI_MAX_SLOTS_TRACKED];

    // Bulk endpoint state for USB Mass Storage. Second index: [0]=OUT, [1]=IN.
    uint16_t bulk_enqueue[XHCI_MAX_SLOTS_TRACKED][2];
    uint8_t  bulk_cycle[XHCI_MAX_SLOTS_TRACKED][2];
    uint8_t  bulk_dci[XHCI_MAX_SLOTS_TRACKED][2];  // doorbell target (endpoint index)
    bool     msc_active[XHCI_MAX_SLOTS_TRACKED];
    uint32_t msc_tag;                               // BOT command tag counter

    // Saved MMIO sub-block pointers so the IRQ handler / runtime poll can reach
    // the operational/runtime/doorbell registers after init returns.
    volatile uint8_t *op_regs;
    volatile uint8_t *runtime_regs;
    volatile uint8_t *doorbell_regs;
    // Scratch buffer used by GET_DESCRIPTOR to receive DMA'd descriptor data.
    uint8_t  xfr_buf[XHCI_MAX_SLOTS_TRACKED][64] __attribute__((aligned(64)));

    uint64_t dcbaa[256] __attribute__((aligned(64)));
    struct xhci_trb cmd_ring[XHCI_CMD_RING_TRBS] __attribute__((aligned(64)));
    struct xhci_trb evt_ring[XHCI_EVENT_RING_TRBS] __attribute__((aligned(64)));
    struct xhci_erst_entry erst[1] __attribute__((aligned(64)));

    // One input/device context pair plus a small EP0 transfer ring per slot.
    uint8_t input_ctx[XHCI_MAX_SLOTS_TRACKED][1024] __attribute__((aligned(64)));
    uint8_t device_ctx[XHCI_MAX_SLOTS_TRACKED][1024] __attribute__((aligned(64)));
    struct xhci_trb ep0_ring[XHCI_MAX_SLOTS_TRACKED][XHCI_EP0_RING_TRBS]
        __attribute__((aligned(64)));
    // Interrupt-IN transfer ring for HID/interrupt endpoints.
    struct xhci_trb intin_ring[XHCI_MAX_SLOTS_TRACKED][XHCI_INT_IN_RING_TRBS]
        __attribute__((aligned(64)));
    // Per-slot ring of distinct 64-byte receive buffers for interrupt-IN reports.
    uint8_t  hid_buf[XHCI_MAX_SLOTS_TRACKED][XHCI_INTIN_NUM_BUFS][64]
        __attribute__((aligned(64)));

    // Bulk transfer rings (OUT, IN) plus BOT command/status wrappers and a
    // 4 KB data bounce. The bounce is 4 KB-aligned so a single data-stage TRB
    // never crosses a 64 KB boundary (an xHCI TRB buffer restriction).
    struct xhci_trb bulk_ring[XHCI_MAX_SLOTS_TRACKED][2][XHCI_BULK_RING_TRBS]
        __attribute__((aligned(64)));
    uint8_t  msc_cbw[XHCI_MAX_SLOTS_TRACKED][64] __attribute__((aligned(64)));  // 31-byte CBW
    uint8_t  msc_csw[XHCI_MAX_SLOTS_TRACKED][64] __attribute__((aligned(64)));  // 13-byte CSW
    uint8_t  msc_data[XHCI_MAX_SLOTS_TRACKED][MSC_XFER_BYTES]
        __attribute__((aligned(4096)));
};

static struct xhci_runtime_state g_xhci_runtime[XHCI_MAX_CONTROLLERS];

static void xhci_bzero(void *ptr, uint64_t len) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    for (uint64_t i = 0; i < len; i++) {
        p[i] = 0;
    }
}

static inline uint8_t xhci_read8(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint8_t *)(base + off);
}

static inline uint16_t xhci_read16(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint16_t *)(base + off);
}

static inline uint32_t xhci_read32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void xhci_write32(volatile uint8_t *base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static inline void xhci_write64(volatile uint8_t *base, uint32_t off, uint64_t val) {
    xhci_write32(base, off, (uint32_t)(val & 0xFFFFFFFFULL));
    xhci_write32(base, off + 4, (uint32_t)(val >> 32));
}

// Translate a kernel-image pointer (all xHCI DMA structures live in the static
// g_xhci_runtime BSS bundle) to the physical address the controller must use for
// DMA. Every address handed to the controller — register writes (DCBAAP, CRCR,
// ERSTBA, ERDP), TRB ring-segment/Link pointers, context dequeue pointers and
// data-buffer pointers — must go through this, or the controller reads/writes
// the wrong physical memory and every command/transfer silently times out.
static inline uint64_t xhci_dma(const volatile void *p) {
    return KV2P((uint64_t)(uintptr_t)p);
}

// Pick or allocate per-controller runtime state by BDF.
static struct xhci_runtime_state *xhci_get_runtime_state(struct usb_controller *ctrl) {
    for (uint32_t i = 0; i < XHCI_MAX_CONTROLLERS; i++) {
        struct xhci_runtime_state *s = &g_xhci_runtime[i];
        if (s->used && s->bus == ctrl->pci.bus &&
            s->device == ctrl->pci.device && s->function == ctrl->pci.function) {
            return s;
        }
    }

    for (uint32_t i = 0; i < XHCI_MAX_CONTROLLERS; i++) {
        struct xhci_runtime_state *s = &g_xhci_runtime[i];
        if (!s->used) {
            xhci_bzero(s, sizeof(*s));
            s->used = true;
            s->bus = ctrl->pci.bus;
            s->device = ctrl->pci.device;
            s->function = ctrl->pci.function;
            return s;
        }
    }

    return NULL;
}

// Busy-wait helper for simple early-boot polling paths.
static bool xhci_wait_for_bits(volatile uint8_t *base, uint32_t reg,
                               uint32_t mask, uint32_t want_set,
                               uint32_t iters) {
    for (uint32_t i = 0; i < iters; i++) {
        uint32_t v = xhci_read32(base, reg);
        if (want_set) {
            if ((v & mask) == mask) {
                return true;
            }
        } else {
            if ((v & mask) == 0) {
                return true;
            }
        }
    }
    return false;
}

static uint8_t xhci_port_speed(volatile uint8_t *op, uint32_t port_index) {
    uint32_t reg = XHCI_PORTSC_BASE + (port_index * XHCI_PORT_STRIDE);
    uint32_t portsc = xhci_read32(op, reg);
    return (uint8_t)((portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT);
}

static uint32_t xhci_ep0_max_packet(uint8_t speed) {
    // Conservative defaults for pre-descriptor setup stage.
    if (speed >= 4U) {
        return 512U; // SuperSpeed default EP0 MPS
    }
    return 64U; // HS/FS default and common fallback for early setup
}

static uint32_t xhci_find_first_connected_port(volatile uint8_t *op, uint32_t max_ports) {
    uint32_t ports_to_scan = (max_ports > 255U) ? 255U : max_ports;
    for (uint32_t p = 0; p < ports_to_scan; p++) {
        uint32_t reg = XHCI_PORTSC_BASE + (p * XHCI_PORT_STRIDE);
        uint32_t portsc = xhci_read32(op, reg);
        if (portsc & XHCI_PORTSC_CCS) {
            return p + 1U; // xHCI root ports are 1-based in slot context
        }
    }
    return 0;
}

// Minimal xHCI startup sequence: stop -> reset -> wait ready -> run.
static bool xhci_controller_reset_and_run(volatile uint8_t *op) {
    uint32_t cmd = xhci_read32(op, XHCI_OP_USBCMD);
    cmd &= ~XHCI_USBCMD_RUN_STOP;
    xhci_write32(op, XHCI_OP_USBCMD, cmd);

    if (!xhci_wait_for_bits(op, XHCI_OP_USBSTS, XHCI_USBSTS_HCH, 1, 2000000)) {
        kprintf("xHCI: timeout waiting HCHalted before reset\n");
        return false;
    }

    cmd = xhci_read32(op, XHCI_OP_USBCMD);
    cmd |= XHCI_USBCMD_HCRST;
    xhci_write32(op, XHCI_OP_USBCMD, cmd);

    if (!xhci_wait_for_bits(op, XHCI_OP_USBCMD, XHCI_USBCMD_HCRST, 0, 4000000)) {
        kprintf("xHCI: timeout waiting HCRST clear\n");
        return false;
    }

    if (!xhci_wait_for_bits(op, XHCI_OP_USBSTS, XHCI_USBSTS_CNR, 0, 4000000)) {
        kprintf("xHCI: timeout waiting CNR clear\n");
        return false;
    }

    cmd = xhci_read32(op, XHCI_OP_USBCMD);
    cmd |= (XHCI_USBCMD_RUN_STOP | XHCI_USBCMD_INTE);
    xhci_write32(op, XHCI_OP_USBCMD, cmd);

    if (!xhci_wait_for_bits(op, XHCI_OP_USBSTS, XHCI_USBSTS_HCH, 0, 2000000)) {
        kprintf("xHCI: timeout waiting run state\n");
        return false;
    }

    return true;
}

static void xhci_ring_doorbell(volatile uint8_t *db, uint32_t target, uint32_t val) {
    xhci_write32(db, target * 4U, val);
}

// Configure DCBAA, command ring, and interrupter-0 event ring.
static bool xhci_setup_rings(struct xhci_runtime_state *rt,
                             volatile uint8_t *op,
                             volatile uint8_t *runtime) {
    if (!rt || !op || !runtime) {
        return false;
    }

    xhci_bzero(rt->dcbaa, sizeof(rt->dcbaa));
    xhci_bzero(rt->cmd_ring, sizeof(rt->cmd_ring));
    xhci_bzero(rt->evt_ring, sizeof(rt->evt_ring));
    xhci_bzero(rt->erst, sizeof(rt->erst));

    rt->cmd_cycle = 1;
    rt->evt_cycle = 1;
    rt->cmd_enqueue = 0;
    rt->evt_dequeue = 0;

    // Command ring is circular via a Link TRB in the final slot.
    struct xhci_trb *link = &rt->cmd_ring[XHCI_CMD_RING_TRBS - 1U];
    uint64_t cmd_ring_base = xhci_dma(&rt->cmd_ring[0]);
    link->d0 = (uint32_t)(cmd_ring_base & 0xFFFFFFFFULL);
    link->d1 = (uint32_t)(cmd_ring_base >> 32);
    link->d2 = 0;
    link->d3 = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) |
               XHCI_TRB_TC_BIT | XHCI_TRB_CYCLE_BIT;

    rt->erst[0].ring_segment_base = xhci_dma(&rt->evt_ring[0]);
    rt->erst[0].ring_segment_size = XHCI_EVENT_RING_TRBS;

    // All controller-visible addresses are physical (see xhci_dma()).
    xhci_write64(op, XHCI_OP_DCBAAP, xhci_dma(&rt->dcbaa[0]));
    xhci_write64(op, XHCI_OP_CRCR, cmd_ring_base | 1ULL);

    volatile uint8_t *ir = runtime + XHCI_RT_IR0;
    xhci_write32(ir, XHCI_IR_ERSTSZ, 1);
    xhci_write64(ir, XHCI_IR_ERSTBA, xhci_dma(&rt->erst[0]));
    xhci_write64(ir, XHCI_IR_ERDP, xhci_dma(&rt->evt_ring[0]));
    xhci_write32(ir, XHCI_IR_IMAN, XHCI_IMAN_IE);

    return true;
}

// Queue an Enable Slot command and ring Doorbell 0 (command ring).
static bool xhci_submit_enable_slot(struct xhci_runtime_state *rt,
                                    volatile uint8_t *doorbell) {
    if (!rt || !doorbell) {
        return false;
    }

    if (rt->cmd_enqueue >= (XHCI_CMD_RING_TRBS - 1U)) {
        return false;
    }

    struct xhci_trb *trb = &rt->cmd_ring[rt->cmd_enqueue];
    trb->d0 = 0;
    trb->d1 = 0;
    trb->d2 = 0;
    trb->d3 = (XHCI_TRB_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT) |
              (rt->cmd_cycle ? XHCI_TRB_CYCLE_BIT : 0U);

    rt->cmd_enqueue++;
    if (rt->cmd_enqueue == (XHCI_CMD_RING_TRBS - 1U)) {
        rt->cmd_enqueue = 0;
        rt->cmd_cycle ^= 1U;
    }

    xhci_ring_doorbell(doorbell, 0, 0);
    return true;
}

static bool xhci_submit_address_device(struct xhci_runtime_state *rt,
                                       volatile uint8_t *doorbell,
                                       uint8_t slot_id,
                                       uint64_t input_ctx_ptr) {
    if (!rt || !doorbell || slot_id == 0 || input_ctx_ptr == 0) {
        return false;
    }

    if (rt->cmd_enqueue >= (XHCI_CMD_RING_TRBS - 1U)) {
        return false;
    }

    struct xhci_trb *trb = &rt->cmd_ring[rt->cmd_enqueue];
    trb->d0 = (uint32_t)(input_ctx_ptr & 0xFFFFFFFFULL);
    trb->d1 = (uint32_t)(input_ctx_ptr >> 32);
    trb->d2 = 0;
    trb->d3 = (XHCI_TRB_ADDRESS_DEVICE << XHCI_TRB_TYPE_SHIFT) |
              ((uint32_t)slot_id << 24) |
              (rt->cmd_cycle ? XHCI_TRB_CYCLE_BIT : 0U);

    rt->cmd_enqueue++;
    if (rt->cmd_enqueue == (XHCI_CMD_RING_TRBS - 1U)) {
        rt->cmd_enqueue = 0;
        rt->cmd_cycle ^= 1U;
    }

    xhci_ring_doorbell(doorbell, 0, 0);
    return true;
}

// Poll event ring for a command completion event from interrupter 0.
static bool xhci_poll_cmd_completion(struct xhci_runtime_state *rt,
                                     volatile uint8_t *runtime,
                                     uint32_t max_iters,
                                     uint8_t *out_code,
                                     uint8_t *out_slot_id) {
    if (!rt || !runtime) {
        return false;
    }

    volatile uint8_t *ir = runtime + XHCI_RT_IR0;

    for (uint32_t i = 0; i < max_iters; i++) {
        struct xhci_trb *ev = &rt->evt_ring[rt->evt_dequeue];
        uint32_t d3 = ev->d3;
        uint32_t cycle = d3 & 1U;
        if (cycle != rt->evt_cycle) {
            continue;
        }

        uint32_t type = (d3 >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        uint8_t cc = (uint8_t)((ev->d2 >> 24) & 0xFFU);
        uint8_t slot_id = (uint8_t)((d3 >> 24) & 0xFFU);

        rt->evt_dequeue++;
        if (rt->evt_dequeue >= XHCI_EVENT_RING_TRBS) {
            rt->evt_dequeue = 0;
            rt->evt_cycle ^= 1U;
        }

        // Tell xHCI where software has consumed up to in the event ring.
        xhci_write64(ir, XHCI_IR_ERDP,
                     xhci_dma(&rt->evt_ring[rt->evt_dequeue]) | (1ULL << 3));

        if (type != XHCI_TRB_CMD_COMPLETION) {
            continue;
        }

        if (out_code) {
            *out_code = cc;
        }
        if (out_slot_id) {
            *out_slot_id = slot_id;
        }
        return true;
    }

    return false;
}

static bool xhci_prepare_address_device_context(struct xhci_runtime_state *rt,
                                                uint8_t slot_id,
                                                uint8_t root_port,
                                                uint8_t port_speed) {
    if (!rt || slot_id == 0) {
        return false;
    }

    uint32_t idx = (uint32_t)slot_id - 1U;
    if (idx >= rt->tracked_slots) {
        return false;
    }

    // xHCI context layout depends on CSZ (32 or 64 bytes per context).
    uint32_t csz = rt->context_size;
    uint8_t *ictx = rt->input_ctx[idx];
    uint8_t *dctx = rt->device_ctx[idx];
    struct xhci_trb *ep0 = rt->ep0_ring[idx];

    xhci_bzero(ictx, 1024);
    xhci_bzero(dctx, 1024);
    xhci_bzero(ep0, sizeof(rt->ep0_ring[idx]));

    // EP0 ring is circular via a Link TRB in the last slot.
    struct xhci_trb *link = &ep0[XHCI_EP0_RING_TRBS - 1U];
    uint64_t ep0_base = xhci_dma(&ep0[0]);
    link->d0 = (uint32_t)(ep0_base & 0xFFFFFFFFULL);
    link->d1 = (uint32_t)(ep0_base >> 32);
    link->d2 = 0;
    // Link cycle starts CLEAR (opposite the initial producer cycle of 1): the
    // enqueue wrap XORs it, so on the first wrap it flips to 1 to match the
    // controller's consumer cycle. Starting it set would desync on first wrap.
    link->d3 = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC_BIT;

    // Input Control Context: Add Slot Context (bit0) and EP0 Context (bit1).
    uint32_t *icc = (uint32_t *)ictx;
    icc[0] = 0;      // drop flags
    icc[1] = 0x3U;   // add flags: slot + ep0

    // Slot Context in input context index 1.
    uint32_t *slot_ctx = (uint32_t *)(ictx + csz * 1U);
    slot_ctx[0] = ((uint32_t)port_speed << 20) | (1U << 27); // speed + context entries
    slot_ctx[1] = ((uint32_t)root_port << 16);               // root hub port number

    // EP0 Context in input context index 2.
    uint32_t *ep0_ctx = (uint32_t *)(ictx + csz * 2U);
    uint32_t mps = xhci_ep0_max_packet(port_speed);
    ep0_ctx[1] = (mps << 16) | (4U << 3) | (3U << 1); // EP type=Control, CErr=3
    ep0_ctx[2] = (uint32_t)(ep0_base & 0xFFFFFFFFULL) | 1U; // DCS=1
    ep0_ctx[3] = (uint32_t)(ep0_base >> 32);
    ep0_ctx[4] = 8U; // average TRB length

    // DCBAA[slot_id] points to this slot's output device context.
    rt->dcbaa[slot_id] = xhci_dma(dctx);
    rt->slot_to_port[idx] = root_port;
    rt->slot_speed[idx] = port_speed;
    rt->slot_active[idx] = 1;

    return true;
}

// -------------------------------------------------------------------------
// EP0 CONTROL TRANSFERS (Setup → Data → Status three-stage pipeline)
// -------------------------------------------------------------------------
// xHCI EP0 transfers use three TRB types placed on the transfer ring that
// was established in xhci_prepare_address_device_context:
//
//   SETUP   (type 2) — carries the 8-byte USB SETUP packet inline in d0/d1.
//   DATA    (type 3) — points at a DMA buffer for IN data (present if wLength>0).
//   STATUS  (type 4) — zero-length opposite-direction handshake; IOC set here
//                      so only one Transfer Event arrives per transaction.
//
// After writing all TRBs, ring Doorbell[slot_id] with endpoint target 1 (EP0).

#define XHCI_TRB_SETUP   2U
#define XHCI_TRB_DATA    3U
#define XHCI_TRB_STATUS  4U
#define XHCI_TRB_TRANSFER_EVENT 32U

// Transfer flags used in d3.
#define XHCI_TRB_IOC     (1U << 5)   // Interrupt on Completion
#define XHCI_TRB_IDT     (1U << 6)   // Immediate Data (Setup TRB only)
#define XHCI_TRB_DIR_IN  (1U << 16)  // Transfer Direction IN (Data/Status)
#define XHCI_TRB_TRT_IN  (3U << 16)  // Transfer Type = IN  (Setup d3[17:16])
#define XHCI_TRB_TRT_NO  (0U << 16)  // Transfer Type = No data stage

// USB SETUP request types / requests (standard device request subset).
#define USB_BMRT_H2D_STD_DEV   0x00U  // host-to-device, standard, device
#define USB_BMRT_D2H_STD_DEV   0x80U  // device-to-host, standard, device
// USB_REQ_GET_DESCRIPTOR already defined in usb.h
#define USB_REQ_SET_ADDRESS_CMD 0x05U // kept distinct from the usb.h constant name
#define USB_DESC_DEVICE_TYPE   0x01U
#define USB_DESC_CONFIG_TYPE   0x02U

// USB class codes for class-driver dispatch.
#define USB_CLASS_HID   0x03U
#define USB_CLASS_MSC   0x08U
#define USB_CLASS_HUB   0x09U

// HID boot-protocol subclass/protocol.
#define USB_HID_SUBCLASS_BOOT  0x01U
#define USB_HID_PROTO_KBD      0x01U
#define USB_HID_PROTO_MOUSE    0x02U

// Enqueue a single TRB on a transfer ring, advancing enqueue/cycle correctly.
// Returns false if the ring is full (last slot is the Link TRB, never used).
static bool xhci_ep0_enqueue_trb(struct xhci_runtime_state *rt,
                                  uint8_t slot_id,
                                  uint32_t d0, uint32_t d1,
                                  uint32_t d2, uint32_t d3_no_cycle) {
    if (!rt || slot_id == 0) { return false; }
    uint32_t idx = (uint32_t)slot_id - 1U;
    if (rt->ep0_enqueue[idx] >= (XHCI_EP0_RING_TRBS - 1U)) { return false; }

    uint16_t pos = rt->ep0_enqueue[idx];
    struct xhci_trb *trb = &rt->ep0_ring[idx][pos];
    trb->d0 = d0;
    trb->d1 = d1;
    trb->d2 = d2;
    // Stamp the current cycle bit last to make the TRB visible atomically.
    trb->d3 = d3_no_cycle | (rt->ep0_cycle[idx] ? XHCI_TRB_CYCLE_BIT : 0U);

    rt->ep0_enqueue[idx]++;
    if (rt->ep0_enqueue[idx] == (XHCI_EP0_RING_TRBS - 1U)) {
        // Wrap: toggle Link TRB cycle bit to toggle the ring's cycle state.
        struct xhci_trb *link = &rt->ep0_ring[idx][XHCI_EP0_RING_TRBS - 1U];
        link->d3 ^= XHCI_TRB_CYCLE_BIT;
        rt->ep0_enqueue[idx] = 0;
        rt->ep0_cycle[idx]   ^= 1U;
    }
    return true;
}

// --------------------------------------------------------------------------
// Generic EP0 control-transfer helper shared by all request builders.
// Packages Setup+[Data]+Status into the ring and polls for a Transfer Event.
// `in_dir`   true  → device-to-host (GET_*)  DATA TRB uses DIR_IN.
// `in_dir`   false → host-to-device (SET_*)  no DATA stage (wLength must be 0).
// `buf/blen` must be NULL/0 for host-to-device requests.
// --------------------------------------------------------------------------
static bool xhci_ep0_control_transfer(struct xhci_runtime_state *rt,
                                       volatile uint8_t *runtime,
                                       volatile uint8_t *doorbell,
                                       uint8_t slot_id,
                                       uint32_t setup_d0, uint32_t setup_d1,
                                       bool in_dir,
                                       uint8_t *buf, uint32_t blen) {
    if (!rt || slot_id == 0) { return false; }

    // SETUP TRB — 8 bytes of USB SETUP data packed in d0/d1 (IDT=1).
    uint32_t trt = in_dir ? XHCI_TRB_TRT_IN : XHCI_TRB_TRT_NO;
    uint32_t setup_d3 = (XHCI_TRB_SETUP << XHCI_TRB_TYPE_SHIFT)
                       | XHCI_TRB_IDT | trt;
    if (!xhci_ep0_enqueue_trb(rt, slot_id, setup_d0, setup_d1, 8U, setup_d3)) {
        return false;
    }

    // DATA TRB — only present for IN transfers with a receive buffer.
    if (in_dir && buf && blen > 0) {
        uint64_t phys = xhci_dma(buf);
        uint32_t dd3  = (XHCI_TRB_DATA << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_DIR_IN;
        if (!xhci_ep0_enqueue_trb(rt, slot_id,
                                   (uint32_t)(phys & 0xFFFFFFFFULL),
                                   (uint32_t)(phys >> 32),
                                   blen, dd3)) {
            return false;
        }
    }

    // STATUS TRB — zero-length handshake in the opposite direction; IOC fires
    // one Transfer Event to signal the whole transaction is done.
    uint32_t status_d3 = (XHCI_TRB_STATUS << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
    if (!xhci_ep0_enqueue_trb(rt, slot_id, 0, 0, 0, status_d3)) {
        return false;
    }

    // Ring EP0 doorbell: doorbell target 1 = control endpoint 0.
    xhci_ring_doorbell(doorbell, (uint32_t)slot_id, 1U);

    // Poll interrupter-0 event ring for our Transfer Event.
    volatile uint8_t *ir = runtime + XHCI_RT_IR0;
    for (uint32_t i = 0; i < 8000000U; i++) {
        struct xhci_trb *ev = &rt->evt_ring[rt->evt_dequeue];
        uint32_t d3 = ev->d3;
        if ((d3 & 1U) != rt->evt_cycle) { continue; }

        uint32_t type    = (d3 >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        uint8_t  cc      = (uint8_t)((ev->d2 >> 24) & 0xFFU);
        uint8_t  ev_slot = (uint8_t)((d3 >> 24) & 0xFFU);

        rt->evt_dequeue++;
        if (rt->evt_dequeue >= XHCI_EVENT_RING_TRBS) {
            rt->evt_dequeue = 0;
            rt->evt_cycle ^= 1U;
        }
        xhci_write64(ir, XHCI_IR_ERDP,
            xhci_dma(&rt->evt_ring[rt->evt_dequeue]) | (1ULL << 3));

        if (type != XHCI_TRB_TRANSFER_EVENT || ev_slot != slot_id) { continue; }
        // Success=1, Short Packet=13 (acceptable for variable-length reads).
        return (cc == 1U || cc == 13U);
    }
    return false;
}

// Issue a GET_DESCRIPTOR(Device, index 0) on EP0 of the addressed slot.
// Descriptor bytes land in rt->xfr_buf[slot_id-1].  Returns false on any
// submission or timeout failure; completion code is logged regardless.
static bool xhci_get_device_descriptor(struct xhci_runtime_state *rt,
                                        volatile uint8_t *runtime,
                                        volatile uint8_t *doorbell,
                                        uint8_t slot_id) {
    if (!rt || slot_id == 0) { return false; }
    uint32_t idx = (uint32_t)slot_id - 1U;

    uint8_t *buf = rt->xfr_buf[idx];
    xhci_bzero(buf, 64);

    // SETUP packet: bmRequestType=0x80, bRequest=GET_DESCRIPTOR,
    // wValue=(DEVICE<<8|0), wIndex=0, wLength=18.
    uint32_t d0 = (uint32_t)USB_BMRT_D2H_STD_DEV
                | ((uint32_t)USB_REQ_GET_DESCRIPTOR << 8)
                | ((uint32_t)USB_DESC_DEVICE_TYPE   << 24);
    uint32_t d1 = (18U << 16); // wIndex=0, wLength=18

    bool ok = xhci_ep0_control_transfer(rt, runtime, doorbell, slot_id,
                                        d0, d1, true, buf, 18U);
    if (!ok) {
        kprintf("xHCI slot%u: GET_DESCRIPTOR(Device) failed\n",
                (unsigned int)slot_id);
        return false;
    }

    kprintf("xHCI slot%u: Device descriptor bLength=%u bcdUSB=%x"
            " bDevClass=%u idVendor=%x idProduct=%x\n",
            (unsigned int)slot_id,
            (unsigned int)buf[0],
            (unsigned int)(buf[2] | ((uint32_t)buf[3] << 8)),
            (unsigned int)buf[4],
            (unsigned int)(buf[8]  | ((uint32_t)buf[9]  << 8)),
            (unsigned int)(buf[10] | ((uint32_t)buf[11] << 8)));
    return true;
}

// Send SET_ADDRESS.  In xHCI the host controller manages the USB address
// internally after Address Device; we still issue the SET_ADDRESS request
// so the device's own address register is programmed correctly.
// `addr` is the new USB device address (1–127).
static bool xhci_set_address(struct xhci_runtime_state *rt,
                              volatile uint8_t *runtime,
                              volatile uint8_t *doorbell,
                              uint8_t slot_id, uint8_t addr) {
    // bmRequestType=0x00 host-to-device, bRequest=SET_ADDRESS,
    // wValue=addr, wIndex=0, wLength=0 — no DATA stage.
    uint32_t d0 = (uint32_t)USB_BMRT_H2D_STD_DEV
                | ((uint32_t)USB_REQ_SET_ADDRESS_CMD << 8)
                | ((uint32_t)addr << 16); // wValue = new address
    uint32_t d1 = 0;                      // wIndex=0, wLength=0
    bool ok = xhci_ep0_control_transfer(rt, runtime, doorbell, slot_id,
                                        d0, d1, false, NULL, 0);
    if (!ok) {
        kprintf("xHCI slot%u: SET_ADDRESS(%u) failed\n",
                (unsigned int)slot_id, (unsigned int)addr);
    } else {
        kprintf("xHCI slot%u: SET_ADDRESS(%u) OK\n",
                (unsigned int)slot_id, (unsigned int)addr);
    }
    return ok;
}

// Send SET_CONFIGURATION to move the device from the Address state into the
// Configured state. Until this succeeds the device's interfaces/endpoints are
// inactive and it produces no interrupt-IN data — so a HID keyboard sends no
// reports. `cfg` is the bConfigurationValue from the configuration descriptor.
static bool xhci_set_configuration(struct xhci_runtime_state *rt,
                                   volatile uint8_t *runtime,
                                   volatile uint8_t *doorbell,
                                   uint8_t slot_id, uint8_t cfg) {
    // bmRequestType=0x00 host-to-device, bRequest=SET_CONFIGURATION,
    // wValue=cfg, wIndex=0, wLength=0 — no DATA stage.
    uint32_t d0 = (uint32_t)USB_BMRT_H2D_STD_DEV
                | ((uint32_t)USB_REQ_SET_CONFIGURATION << 8)
                | ((uint32_t)cfg << 16); // wValue = configuration value
    uint32_t d1 = 0;                     // wIndex=0, wLength=0
    bool ok = xhci_ep0_control_transfer(rt, runtime, doorbell, slot_id,
                                        d0, d1, false, NULL, 0);
    kprintf("xHCI slot%u: SET_CONFIGURATION(%u) %s\n",
            (unsigned int)slot_id, (unsigned int)cfg, ok ? "OK" : "failed");
    return ok;
}

// Fetch GET_DESCRIPTOR(Configuration, index 0) — first pass with 9-byte
// header only to discover wTotalLength, then a full read.
// The full config blob is stored in rt->xfr_buf[slot_id-1] (up to 64 bytes).
static bool xhci_get_config_descriptor(struct xhci_runtime_state *rt,
                                        volatile uint8_t *runtime,
                                        volatile uint8_t *doorbell,
                                        uint8_t slot_id) {
    uint32_t idx = (uint32_t)slot_id - 1U;
    uint8_t *buf = rt->xfr_buf[idx];
    xhci_bzero(buf, 64);

    // First fetch: 9 bytes (Configuration Descriptor header only).
    uint32_t d0 = (uint32_t)USB_BMRT_D2H_STD_DEV
                | ((uint32_t)USB_REQ_GET_DESCRIPTOR << 8)
                | ((uint32_t)USB_DESC_CONFIG_TYPE   << 24);
    uint32_t d1 = (9U << 16); // wIndex=0, wLength=9

    if (!xhci_ep0_control_transfer(rt, runtime, doorbell, slot_id,
                                   d0, d1, true, buf, 9U)) {
        kprintf("xHCI slot%u: GET_DESCRIPTOR(Config) header failed\n",
                (unsigned int)slot_id);
        return false;
    }

    uint16_t total_len = (uint16_t)(buf[2] | ((uint32_t)buf[3] << 8));
    kprintf("xHCI slot%u: Config descriptor wTotalLength=%u bNumInterfaces=%u"
            " bConfigValue=%u\n",
            (unsigned int)slot_id,
            (unsigned int)total_len,
            (unsigned int)buf[4],  // bNumInterfaces
            (unsigned int)buf[5]); // bConfigurationValue

    // Second fetch: full descriptor up to our buffer limit.
    if (total_len > 64U) { total_len = 64U; }
    if (total_len > 9U) {
        xhci_bzero(buf, 64);
        uint32_t d1b = ((uint32_t)total_len << 16);
        if (!xhci_ep0_control_transfer(rt, runtime, doorbell, slot_id,
                                       d0, d1b, true, buf, total_len)) {
            kprintf("xHCI slot%u: GET_DESCRIPTOR(Config) full fetch failed\n",
                    (unsigned int)slot_id);
            return false;
        }
    }
    return true;
}

// Forward declarations for helpers used by xhci_dispatch_class before they
// are fully defined (defined later in this file after the dispatch function).
static bool xhci_find_intin_endpoint(struct xhci_runtime_state *rt,
                                      uint8_t slot_id,
                                      uint8_t *out_ep_addr,
                                      uint8_t *out_interval,
                                      uint16_t *out_mps);
static bool xhci_configure_intin_endpoint(struct xhci_runtime_state *rt,
                                           volatile uint8_t *op,
                                           volatile uint8_t *runtime,
                                           volatile uint8_t *doorbell,
                                           uint8_t slot_id,
                                           uint8_t ep_addr,
                                           uint16_t mps,
                                           uint8_t interval);
static void xhci_intin_post_buffers(struct xhci_runtime_state *rt,
                                    volatile uint8_t *doorbell,
                                    uint8_t slot_id,
                                    uint16_t mps,
                                    uint32_t count);
static bool xhci_msc_setup(struct xhci_runtime_state *rt, uint8_t slot_id,
                           volatile uint8_t *op, volatile uint8_t *runtime,
                           volatile uint8_t *doorbell);

// Walk the configuration blob in xfr_buf and identify the first interface
// class.  For HID/boot-keyboard devices, log the class details.
static void xhci_dispatch_class(struct xhci_runtime_state *rt,
                                uint8_t slot_id,
                                volatile uint8_t *op_ptr,
                                volatile uint8_t *runtime_ptr,
                                volatile uint8_t *doorbell_ptr) {
    uint32_t idx = (uint32_t)slot_id - 1U;
    uint8_t *buf = rt->xfr_buf[idx];

    // Step through each descriptor in the blob by bLength to find an Interface.
    uint32_t off = 0;
    while (off + 2U <= 64U) {
        uint8_t dlen  = buf[off];
        uint8_t dtype = buf[off + 1U];
        if (dlen == 0U) { break; }  // malformed or end of descriptors

        // Interface Descriptor (type 4): contains class/subclass/protocol.
        if (dtype == USB_DESC_INTERFACE) {
            if (off + 9U <= 64U) {
                uint8_t iclass    = buf[off + 5U];
                uint8_t isubclass = buf[off + 6U];
                uint8_t iproto    = buf[off + 7U];

                kprintf("xHCI slot%u: Interface class=%x sub=%x proto=%x\n",
                        (unsigned int)slot_id,
                        (unsigned int)iclass,
                        (unsigned int)isubclass,
                        (unsigned int)iproto);

                // HID class — boot subclass with keyboard or mouse protocol.
                if (iclass == USB_CLASS_HID &&
                    isubclass == USB_HID_SUBCLASS_BOOT) {
                    if (iproto == USB_HID_PROTO_KBD) {
                        kprintf("xHCI slot%u: HID Boot Keyboard — configuring endpoint\n",
                                (unsigned int)slot_id);
                        // Find the interrupt-IN endpoint address from the blob.
                        uint8_t ep_addr = 0;
                        uint8_t ep_intv = 0;
                        uint16_t ep_mps = 8;
                        if (xhci_find_intin_endpoint(rt, slot_id,
                                                     &ep_addr, &ep_intv, &ep_mps)) {
                            kprintf("xHCI slot%u: interrupt-IN ep=0x%x mps=%u interval=%u\n",
                                    (unsigned int)slot_id,
                                    (unsigned int)ep_addr,
                                    (unsigned int)ep_mps,
                                    (unsigned int)ep_intv);
                            // Store so the runtime poll can ring the right doorbell.
                            uint32_t si = (uint32_t)slot_id - 1U;
                            rt->intin_ep_addr[si] = ep_addr;
                            rt->intin_interval[si] = ep_intv;
                            rt->intin_mps[si] = ep_mps;
                            rt->intin_active[si] = false;
                            // Issue Configure Endpoint to activate the IN ring.
                            if (xhci_configure_intin_endpoint(rt, op_ptr, runtime_ptr,
                                                               doorbell_ptr, slot_id,
                                                               ep_addr, ep_mps, ep_intv)) {
                                rt->intin_active[si] = true;
                                // Prime the ring with several receive buffers; from
                                // now on xhci_poll() (main loop) drains reports and
                                // re-arms buffers without blocking.
                                xhci_intin_post_buffers(rt, doorbell_ptr, slot_id,
                                                        ep_mps, XHCI_INTIN_NUM_BUFS);
                                kprintf("xHCI slot%u: HID keyboard ready — polling via main loop\n",
                                        (unsigned int)slot_id);
                            }
                        } else {
                            kprintf("xHCI slot%u: no interrupt-IN endpoint found\n",
                                    (unsigned int)slot_id);
                        }
                    } else if (iproto == USB_HID_PROTO_MOUSE) {
                        kprintf("xHCI slot%u: HID Boot Mouse detected\n",
                                (unsigned int)slot_id);
                    }
                } else if (iclass == USB_CLASS_MSC) {
                    kprintf("xHCI slot%u: Mass Storage device detected\n",
                            (unsigned int)slot_id);
                    // Configure bulk endpoints, run BOT/SCSI, register a block dev.
                    xhci_msc_setup(rt, slot_id, op_ptr, runtime_ptr, doorbell_ptr);
                } else if (iclass == USB_CLASS_HUB) {
                    kprintf("xHCI slot%u: USB Hub detected\n",
                            (unsigned int)slot_id);
                }
            }
        }

        off += dlen;
    }
}

// --------------------------------------------------------------------------
// USB HID boot-keyboard usage-to-ASCII translation table.
// Index = HID Usage ID (see USB HID Usage Tables §10).  Only the printable
// subset used by a standard 104-key US-QWERTY layout is covered here.
// --------------------------------------------------------------------------
static const char g_hid_usage_to_ascii[256] = {
    0,    0,    0,    0,    'a',  'b',  'c',  'd',  // 00-07
    'e',  'f',  'g',  'h',  'i',  'j',  'k',  'l',  // 08-0F
    'm',  'n',  'o',  'p',  'q',  'r',  's',  't',  // 10-17
    'u',  'v',  'w',  'x',  'y',  'z',  '1',  '2',  // 18-1F
    '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  // 20-27
    '\n', 0x1B, '\b', '\t', ' ',  '-',  '=',  '[',  // 28-2F  Enter/Esc/BS/Tab/Space
    ']',  '\\', 0,    ';',  '\'', '`',  ',',  '.',  // 30-37
    '/',  0,    0,    0,    0,    0,    0,    0,    // 38-3F  CapsLock, F-keys
    0,    0,    0,    0,    0,    0,    0,    0,    // 40-47
    0,    0,    0,    0,    0,    0,    0,    0,    // 48-4F  arrows/ins/del/…
    0,    0,    0,    0,    '/',  '*',  '-',  '+',  // 50-57  numpad
    '\n', '1',  '2',  '3',  '4',  '5',  '6',  '7',  // 58-5F  numpad
    '8',  '9',  '0',  '.', /* rest zero */
};

// --------------------------------------------------------------------------
// Interrupt-IN endpoint ring helper — mirrors xhci_ep0_enqueue_trb but
// targets the per-slot intin_ring rather than the EP0 ring.
// --------------------------------------------------------------------------
static bool xhci_intin_enqueue_trb(struct xhci_runtime_state *rt,
                                    uint8_t slot_id,
                                    uint32_t d0, uint32_t d1,
                                    uint32_t d2, uint32_t d3_no_cycle) {
    if (!rt || slot_id == 0) { return false; }
    uint32_t idx = (uint32_t)slot_id - 1U;
    if (rt->intin_enqueue[idx] >= (XHCI_INT_IN_RING_TRBS - 1U)) { return false; }

    uint16_t pos = rt->intin_enqueue[idx];
    struct xhci_trb *trb = &rt->intin_ring[idx][pos];
    trb->d0 = d0;
    trb->d1 = d1;
    trb->d2 = d2;
    trb->d3 = d3_no_cycle | (rt->intin_cycle[idx] ? XHCI_TRB_CYCLE_BIT : 0U);

    rt->intin_enqueue[idx]++;
    if (rt->intin_enqueue[idx] == (XHCI_INT_IN_RING_TRBS - 1U)) {
        struct xhci_trb *link = &rt->intin_ring[idx][XHCI_INT_IN_RING_TRBS - 1U];
        link->d3 ^= XHCI_TRB_CYCLE_BIT;
        rt->intin_enqueue[idx] = 0;
        rt->intin_cycle[idx]   ^= 1U;
    }
    return true;
}

// --------------------------------------------------------------------------
// Parse the full config descriptor blob (already in rt->xfr_buf[idx]) and
// extract the first interrupt-IN endpoint's address and interval.
// Returns false if no interrupt-IN endpoint was found.
// --------------------------------------------------------------------------
static bool xhci_find_intin_endpoint(struct xhci_runtime_state *rt,
                                      uint8_t slot_id,
                                      uint8_t *out_ep_addr,
                                      uint8_t *out_interval,
                                      uint16_t *out_mps) {
    uint32_t idx = (uint32_t)slot_id - 1U;
    uint8_t *buf = rt->xfr_buf[idx];

    uint32_t off = 0;
    while (off + 2U <= 64U) {
        uint8_t dlen  = buf[off];
        uint8_t dtype = buf[off + 1U];
        if (dlen == 0U) { break; }

        // Endpoint Descriptor (type 5) with bmAttributes bits[1:0] == 0x03
        // (interrupt transfer type) and direction IN (bit 7 of bEndpointAddress).
        if (dtype == USB_DESC_ENDPOINT && off + 7U <= 64U) {
            uint8_t ep_addr = buf[off + 2U];
            uint8_t attrs   = buf[off + 3U];
            uint16_t mps    = (uint16_t)(buf[off + 4U] | ((uint32_t)buf[off + 5U] << 8));
            uint8_t  intv   = buf[off + 6U];

            bool is_in  = (ep_addr & 0x80U) != 0;
            bool is_int = (attrs & 0x03U) == 0x03U;

            if (is_in && is_int) {
                *out_ep_addr = ep_addr;
                *out_interval = intv;
                *out_mps = mps;
                return true;
            }
        }
        off += dlen;
    }
    return false;
}

// --------------------------------------------------------------------------
// Issue a Configure Endpoint command for a single interrupt-IN endpoint.
//
// xHCI Configure Endpoint reuses the input-context mechanism: we add the new
// endpoint context (slot[1] stays unchanged from Address Device; we only add
// the interrupt-IN endpoint's context at slot[ep_id]) and submit the command.
// --------------------------------------------------------------------------
static bool xhci_configure_intin_endpoint(struct xhci_runtime_state *rt,
                                           volatile uint8_t *op,
                                           volatile uint8_t *runtime,
                                           volatile uint8_t *doorbell,
                                           uint8_t slot_id,
                                           uint8_t ep_addr,
                                           uint16_t mps,
                                           uint8_t interval) {
    if (!rt || slot_id == 0) { return false; }
    uint32_t idx   = (uint32_t)slot_id - 1U;
    uint32_t csz   = rt->context_size;

    // xHCI endpoint ID = (ep_address & 0xF)*2 + direction_bit.
    // For an IN endpoint direction_bit=1; for OUT=0.
    uint8_t  ep_num = ep_addr & 0x0FU;
    uint32_t ep_id  = (uint32_t)ep_num * 2U + 1U; // IN direction

    uint8_t *ictx = rt->input_ctx[idx];
    xhci_bzero(ictx, 1024);

    // Input Control Context: add the new endpoint, keep the slot context.
    uint32_t *icc = (uint32_t *)ictx;
    icc[0] = 0;
    icc[1] = (1U << 0U) | (1U << ep_id); // add slot ctx (A0) + new EP ctx

    // Slot Context stays at its existing state (context entry count = ep_id).
    // We read it from device_ctx and copy it into input_ctx slot 1.
    uint8_t *dctx = rt->device_ctx[idx];
    uint32_t *dst_slot = (uint32_t *)(ictx + csz * 1U);
    uint32_t *src_slot = (uint32_t *)dctx;
    for (uint32_t w = 0; w < csz / 4U; w++) { dst_slot[w] = src_slot[w]; }
    // Bump Context Entries field to cover the new endpoint.
    dst_slot[0] = (dst_slot[0] & ~(0x1FU << 27)) | (ep_id << 27);

    // Interrupt-IN Endpoint Context at input context index (ep_id+1).
    // Transfer-ring dequeue pointer, MPS, interval, EP type = interrupt-IN(7).
    xhci_bzero(rt->intin_ring[idx], sizeof(rt->intin_ring[idx]));
    rt->intin_enqueue[idx] = 0;
    rt->intin_cycle[idx]   = 1;
    rt->intin_buf_head[idx] = 0;
    rt->intin_buf_tail[idx] = 0;
    // Link TRB at the last slot.
    struct xhci_trb *link = &rt->intin_ring[idx][XHCI_INT_IN_RING_TRBS - 1U];
    uint64_t ring_base = xhci_dma(&rt->intin_ring[idx][0]);
    link->d0 = (uint32_t)(ring_base & 0xFFFFFFFFULL);
    link->d1 = (uint32_t)(ring_base >> 32);
    link->d2 = 0;
    // Link cycle starts CLEAR — see the EP0 ring note; the wrap XORs it.
    link->d3 = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC_BIT;

    uint32_t *ep_ctx = (uint32_t *)(ictx + csz * (ep_id + 1U));
    ep_ctx[0] = (uint32_t)interval << 16; // Interval
    ep_ctx[1] = (7U << 3) | (3U << 1) | (mps << 16); // EP type=interrupt-IN, CErr=3
    ep_ctx[2] = (uint32_t)(ring_base & 0xFFFFFFFFULL) | 1U; // DCS=1
    ep_ctx[3] = (uint32_t)(ring_base >> 32);
    ep_ctx[4] = (uint32_t)mps; // average TRB length hint

    // Submit Configure Endpoint command.
    if (rt->cmd_enqueue >= (XHCI_CMD_RING_TRBS - 1U)) { return false; }
    uint64_t ictx_phys = xhci_dma(ictx);
    struct xhci_trb *cmd = &rt->cmd_ring[rt->cmd_enqueue];
    cmd->d0 = (uint32_t)(ictx_phys & 0xFFFFFFFFULL);
    cmd->d1 = (uint32_t)(ictx_phys >> 32);
    cmd->d2 = 0;
    cmd->d3 = (XHCI_TRB_CFG_ENDPOINT << XHCI_TRB_TYPE_SHIFT)
            | ((uint32_t)slot_id << 24)
            | (rt->cmd_cycle ? XHCI_TRB_CYCLE_BIT : 0U);
    rt->cmd_enqueue++;
    if (rt->cmd_enqueue == (XHCI_CMD_RING_TRBS - 1U)) {
        rt->cmd_enqueue = 0;
        rt->cmd_cycle ^= 1U;
    }
    xhci_ring_doorbell(doorbell, 0, 0); // ring command ring doorbell

    uint8_t cc = 0xFF, out_slot = 0;
    if (!xhci_poll_cmd_completion(rt, runtime, 4000000, &cc, &out_slot)) {
        kprintf("xHCI slot%u: Configure Endpoint timeout\n", (unsigned int)slot_id);
        return false;
    }
    kprintf("xHCI slot%u: Configure Endpoint code=%u\n",
            (unsigned int)slot_id, (unsigned int)cc);
    return (cc == 1U);
}

// --------------------------------------------------------------------------
// Refill the interrupt-IN ring with Normal TRBs that point at hid_buf so the
// controller always has somewhere to DMA the next report.
// Call once after Configure Endpoint and again whenever a report arrives.
// --------------------------------------------------------------------------
static void xhci_intin_post_buffers(struct xhci_runtime_state *rt,
                                     volatile uint8_t *doorbell,
                                     uint8_t slot_id,
                                     uint16_t mps,
                                     uint32_t count) {
    uint32_t idx   = (uint32_t)slot_id - 1U;
    uint8_t  ep_num = rt->intin_ep_addr[idx] & 0x0FU;
    uint32_t ep_id  = ep_num * 2U + 1U; // IN = doorbell target ep_id + 1? No:
    // xHCI doorbell target for endpoint N = ep_id (1-31, see spec §6.4).
    // endpoint doorbell value = endpoint index (same as ep_id).
    uint32_t db_target = ep_id;

    uint32_t safe = (mps > 64U) ? 64U : mps; // cap to our buffer size

    for (uint32_t i = 0; i < count; i++) {
        // Each TRB points at its OWN buffer (round-robin) so an incoming report
        // can't clobber one we haven't read yet. IOC on every TRB so each report
        // raises a Transfer Event.
        uint8_t bhead = rt->intin_buf_head[idx];
        uint64_t phys = xhci_dma(&rt->hid_buf[idx][bhead][0]);
        rt->intin_buf_head[idx] = (uint8_t)((bhead + 1U) % XHCI_INTIN_NUM_BUFS);

        uint32_t flags = (1U << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC; // Normal + IOC
        xhci_intin_enqueue_trb(rt, slot_id,
                               (uint32_t)(phys & 0xFFFFFFFFULL),
                               (uint32_t)(phys >> 32),
                               safe, flags);
    }
    xhci_ring_doorbell(doorbell, (uint32_t)slot_id, db_target);
}

// --------------------------------------------------------------------------
// Parse the 8-byte HID boot-keyboard report sitting in rt->hid_buf[idx] and
// inject any freshly-pressed keys into the keyboard input buffer.
//   report[0]    = modifier bitmap (Shift/Ctrl/Alt/GUI)
//   report[1]    = reserved
//   report[2..7] = up to 6 concurrent key usages (0 = none)
// rt->prev_keys[idx] holds the previous report's keys so each key is emitted
// only on its press edge, not repeatedly while held. `rep_buf` is the specific
// receive buffer the completed TRB DMA'd into.
// --------------------------------------------------------------------------
static void xhci_process_hid_report(struct xhci_runtime_state *rt, uint32_t idx,
                                    const uint8_t *rep_buf) {
    bool shift = (rep_buf[0] & 0x22U) != 0; // LeftShift(0x02)|RightShift(0x20)

    for (uint32_t k = 2; k < 8; k++) {
        uint8_t usage = rep_buf[k];
        if (usage == 0) { continue; }

        // Skip keycodes already held in the previous report (no auto-repeat).
        bool already = false;
        for (uint32_t p = 0; p < 6; p++) {
            if (rt->prev_keys[idx][p] == usage) { already = true; break; }
        }
        if (already) { continue; }

        char ch = g_hid_usage_to_ascii[usage];
        if (ch == 0) { continue; }

        // Apply shift: uppercase letters, shifted symbols.
        if (shift) {
            if (ch >= 'a' && ch <= 'z') {
                ch = (char)(ch - 32); // to uppercase
            } else {
                // Common shifted symbols on US QWERTY.
                const char shifted[] = ")!@#$%^&*(";
                if (ch >= '0' && ch <= '9') {
                    ch = shifted[ch - '0'];
                } else {
                    switch (ch) {
                    case '-': ch = '_'; break;
                    case '=': ch = '+'; break;
                    case '[': ch = '{'; break;
                    case ']': ch = '}'; break;
                    case '\\': ch = '|'; break;
                    case ';': ch = ':'; break;
                    case '\'': ch = '"'; break;
                    case '`': ch = '~'; break;
                    case ',': ch = '<'; break;
                    case '.': ch = '>'; break;
                    case '/': ch = '?'; break;
                    default:  break;
                    }
                }
            }
        }

        keyboard_inject_char(ch);
    }

    // Remember this report's keycodes for next-report edge detection.
    for (uint32_t k = 0; k < 6; k++) {
        rt->prev_keys[idx][k] = rep_buf[k + 2U];
    }
}

// --------------------------------------------------------------------------
// Non-blocking event-ring service. Drains any Transfer Events the controller
// has posted since the last call; for active HID keyboard slots it parses the
// report and re-arms a fresh receive buffer. Returns as soon as the ring is
// empty, so it is cheap to call from the main loop every tick (no busy-wait).
// The safety cap bounds work per call in case of a runaway event stream.
// --------------------------------------------------------------------------
static void xhci_service_events(struct xhci_runtime_state *rt) {
    volatile uint8_t *runtime  = rt->runtime_regs;
    volatile uint8_t *doorbell = rt->doorbell_regs;
    if (!runtime || !doorbell) { return; }
    volatile uint8_t *ir = runtime + XHCI_RT_IR0;

    for (uint32_t guard = 0; guard < XHCI_EVENT_RING_TRBS; guard++) {
        struct xhci_trb *ev = &rt->evt_ring[rt->evt_dequeue];
        uint32_t d3 = ev->d3;
        if ((d3 & 1U) != rt->evt_cycle) { return; } // ring empty

        uint32_t type    = (d3 >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        uint8_t  cc      = (uint8_t)((ev->d2 >> 24) & 0xFFU);
        uint8_t  ev_slot = (uint8_t)((d3 >> 24) & 0xFFU);

        rt->evt_dequeue++;
        if (rt->evt_dequeue >= XHCI_EVENT_RING_TRBS) {
            rt->evt_dequeue = 0;
            rt->evt_cycle ^= 1U;
        }
        xhci_write64(ir, XHCI_IR_ERDP,
            xhci_dma(&rt->evt_ring[rt->evt_dequeue]) | (1ULL << 3));

        if (type != XHCI_TRB_TRANSFER_EVENT) { continue; }
        if (ev_slot == 0) { continue; }
        uint32_t idx = (uint32_t)ev_slot - 1U;
        if (idx >= XHCI_MAX_SLOTS_TRACKED || !rt->intin_active[idx]) { continue; }

        // The completed TRB DMA'd into the tail buffer (buffers complete in the
        // same FIFO order they were armed). Read that one, then advance tail.
        uint8_t btail = rt->intin_buf_tail[idx];
        const uint8_t *rep = rt->hid_buf[idx][btail];
        rt->intin_buf_tail[idx] = (uint8_t)((btail + 1U) % XHCI_INTIN_NUM_BUFS);

        // Success=1, Short Packet=13 both carry a valid (possibly short) report.
        if (cc == 1U || cc == 13U) {
            xhci_process_hid_report(rt, idx, rep);
        }
        // Re-arm one receive buffer so the ring stays populated.
        xhci_intin_post_buffers(rt, doorbell, ev_slot, rt->intin_mps[idx], 1);
    }
}

// Enable/disable interrupter 0's Interrupt Enable bit. Used to briefly silence
// the xHCI IRQ while a synchronous busy-poll path (MSC block I/O) owns the event
// ring, so the handler can't consume the completion the busy-poll is waiting on.
static void xhci_intr_enable(struct xhci_runtime_state *rt, bool on) {
    if (!rt->runtime_regs) { return; }
    volatile uint8_t *ir = rt->runtime_regs + XHCI_RT_IR0;
    xhci_write32(ir, XHCI_IR_IMAN, on ? XHCI_IMAN_IE : 0U);
}

// Acknowledge a pending interrupt on a controller: clear USBSTS.EINT and the
// interrupter's IMAN.IP (both write-1-to-clear), keeping Interrupt Enable set.
// Must run before the level-triggered line is EOI'd or it re-fires immediately.
static void xhci_ack_interrupt(struct xhci_runtime_state *rt) {
    if (rt->op_regs) {
        xhci_write32(rt->op_regs, XHCI_OP_USBSTS, XHCI_USBSTS_EINT);
    }
    if (rt->runtime_regs) {
        volatile uint8_t *ir = rt->runtime_regs + XHCI_RT_IR0;
        xhci_write32(ir, XHCI_IR_IMAN, XHCI_IMAN_IE | XHCI_IMAN_IP);
    }
}

// Interrupt handler (registered via irq_register). Acknowledges every controller
// and drains its event ring: HID reports are injected into the keyboard buffer
// and receive buffers re-armed. Called with interrupts off; the IRQ dispatcher
// sends the LAPIC EOI afterwards.
void xhci_irq(void) {
    for (uint32_t i = 0; i < XHCI_MAX_CONTROLLERS; i++) {
        struct xhci_runtime_state *rt = &g_xhci_runtime[i];
        if (!rt->used) { continue; }
        xhci_ack_interrupt(rt);
        xhci_service_events(rt);
    }
}

// Route each controller's PCI interrupt (level-triggered, active-low) to its IRQ
// vector and register the shared handler. Call once after enumeration so the
// synchronous busy-poll enumeration path isn't disturbed.
void xhci_enable_irq(void) {
    for (uint32_t i = 0; i < XHCI_MAX_CONTROLLERS; i++) {
        struct xhci_runtime_state *rt = &g_xhci_runtime[i];
        if (!rt->used) { continue; }
        const struct usb_controller *ctrl = NULL;
        for (uint32_t c = 0; c < usb_controller_count(); c++) {
            const struct usb_controller *cc = usb_get_controller(c);
            if (cc && cc->pci.bus == rt->bus && cc->pci.device == rt->device &&
                cc->pci.function == rt->function) { ctrl = cc; break; }
        }
        if (!ctrl) { continue; }
        uint8_t line = ctrl->irq_line;
        if (line >= 16U) {                       // 0xFF = no legacy IRQ line
            kprintf("xHCI %x:%x.%x: no usable IRQ line (%u); staying polled\n",
                    (unsigned int)rt->bus, (unsigned int)rt->device,
                    (unsigned int)rt->function, (unsigned int)line);
            continue;
        }
        uint8_t vector = (uint8_t)(32U + line);   // reuse the IRQ stub at IDT 32+line
        irq_register(line, xhci_irq);             // -> irq_handlers[line] = xhci_irq

        // Prefer message-signalled interrupts (MSI-X, then MSI): the device
        // signals `vector` straight to the LAPIC, avoiding the PIC-vs-APIC
        // PCI-INTx GSI-mapping problem. qemu-xhci exposes MSI-X. Fall back to a
        // level-triggered IOAPIC route on the legacy line as a last resort.
        const char *mode;
        if (pci_enable_msix(rt->bus, rt->device, rt->function, vector, 0)) {
            mode = "MSI-X";
        } else if (pci_enable_msi(rt->bus, rt->device, rt->function, vector, 0)) {
            mode = "MSI";
        } else {
            ioapic_route_level(line, vector, 0, true); // PCI INTx: level, active-low
            mode = "INTx";
        }
        xhci_intr_enable(rt, true);
        kprintf("xHCI %x:%x.%x: interrupt-driven via %s (vector %u)\n",
                (unsigned int)rt->bus, (unsigned int)rt->device,
                (unsigned int)rt->function, mode, (unsigned int)vector);
    }
}

// ==========================================================================
// USB Mass Storage — Bulk-Only Transport (BOT) over two bulk endpoints driving
// a minimal SCSI command set, exposed to the kernel as an embk_block_device.
// ==========================================================================

// Per-device context backing a registered block device (driver_data points here).
struct xhci_msc_dev {
    struct embk_block_device blk;
    struct xhci_runtime_state *rt;
    volatile uint8_t *runtime;
    volatile uint8_t *doorbell;
    uint8_t slot_id;
    bool used;
};
static struct xhci_msc_dev g_msc_devs[XHCI_MAX_CONTROLLERS * XHCI_MAX_SLOTS_TRACKED];

static inline void msc_put_le32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static inline uint32_t msc_get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline uint32_t msc_get_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}

// Enqueue a Normal TRB on a bulk transfer ring (d=0 OUT, d=1 IN).
static bool xhci_bulk_enqueue_trb(struct xhci_runtime_state *rt, uint32_t idx,
                                  uint32_t d, uint32_t d0, uint32_t d1,
                                  uint32_t d2, uint32_t d3_no_cycle) {
    if (rt->bulk_enqueue[idx][d] >= (XHCI_BULK_RING_TRBS - 1U)) { return false; }
    uint16_t pos = rt->bulk_enqueue[idx][d];
    struct xhci_trb *trb = &rt->bulk_ring[idx][d][pos];
    trb->d0 = d0; trb->d1 = d1; trb->d2 = d2;
    trb->d3 = d3_no_cycle | (rt->bulk_cycle[idx][d] ? XHCI_TRB_CYCLE_BIT : 0U);
    rt->bulk_enqueue[idx][d]++;
    if (rt->bulk_enqueue[idx][d] == (XHCI_BULK_RING_TRBS - 1U)) {
        struct xhci_trb *link = &rt->bulk_ring[idx][d][XHCI_BULK_RING_TRBS - 1U];
        link->d3 ^= XHCI_TRB_CYCLE_BIT;
        rt->bulk_enqueue[idx][d] = 0;
        rt->bulk_cycle[idx][d] ^= 1U;
    }
    return true;
}

// Synchronous single-TRB bulk transfer; busy-polls the shared event ring for
// the completion. dir_in: true = bulk-IN, false = bulk-OUT. Returns true on a
// Success (1) or Short Packet (13) completion code.
static bool xhci_bulk_xfer(struct xhci_msc_dev *m, bool dir_in,
                           uint64_t phys, uint32_t len) {
    struct xhci_runtime_state *rt = m->rt;
    uint32_t idx = (uint32_t)m->slot_id - 1U;
    uint32_t d = dir_in ? 1U : 0U;
    uint32_t d3 = (1U << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC; // Normal TRB + IOC
    if (!xhci_bulk_enqueue_trb(rt, idx, d,
                               (uint32_t)(phys & 0xFFFFFFFFULL),
                               (uint32_t)(phys >> 32), len, d3)) {
        return false;
    }
    xhci_ring_doorbell(m->doorbell, m->slot_id, rt->bulk_dci[idx][d]);

    volatile uint8_t *ir = m->runtime + XHCI_RT_IR0;
    for (uint32_t i = 0; i < 30000000U; i++) {
        struct xhci_trb *ev = &rt->evt_ring[rt->evt_dequeue];
        if ((ev->d3 & 1U) != rt->evt_cycle) { continue; }
        uint32_t type    = (ev->d3 >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        uint8_t  cc      = (uint8_t)((ev->d2 >> 24) & 0xFFU);
        uint8_t  ev_slot = (uint8_t)((ev->d3 >> 24) & 0xFFU);
        rt->evt_dequeue++;
        if (rt->evt_dequeue >= XHCI_EVENT_RING_TRBS) { rt->evt_dequeue = 0; rt->evt_cycle ^= 1U; }
        xhci_write64(ir, XHCI_IR_ERDP, xhci_dma(&rt->evt_ring[rt->evt_dequeue]) | (1ULL << 3));
        if (type != XHCI_TRB_TRANSFER_EVENT || ev_slot != m->slot_id) { continue; }
        return (cc == 1U || cc == 13U);
    }
    return false;
}

// One Bulk-Only Transport transaction: CBW (out) -> optional data -> CSW (in).
// The data stage always uses the per-slot 4 KB bounce buffer msc_data[idx].
static bool xhci_msc_bot(struct xhci_msc_dev *m, const uint8_t *cdb, uint8_t cdb_len,
                         bool data_in, uint32_t data_len) {
    struct xhci_runtime_state *rt = m->rt;
    uint32_t idx = (uint32_t)m->slot_id - 1U;
    if (data_len > MSC_XFER_BYTES) { return false; }

    // --- Command stage: build + send the 31-byte Command Block Wrapper. ---
    uint8_t *cbw = rt->msc_cbw[idx];
    xhci_bzero(cbw, 31);
    msc_put_le32(cbw + 0, MSC_CBW_SIGNATURE);
    uint32_t tag = ++rt->msc_tag;
    msc_put_le32(cbw + 4, tag);
    msc_put_le32(cbw + 8, data_len);          // dCBWDataTransferLength
    cbw[12] = data_in ? 0x80U : 0x00U;        // bmCBWFlags (bit7: 1=data-IN)
    cbw[13] = 0;                              // bCBWLUN
    cbw[14] = cdb_len;                        // bCBWCBLength
    for (uint8_t i = 0; i < cdb_len && i < 16U; i++) { cbw[15 + i] = cdb[i]; }
    if (!xhci_bulk_xfer(m, false, xhci_dma(cbw), 31U)) { return false; }

    // --- Data stage (optional). ---
    if (data_len > 0U) {
        if (!xhci_bulk_xfer(m, data_in, xhci_dma(rt->msc_data[idx]), data_len)) {
            return false;
        }
    }

    // --- Status stage: read the 13-byte Command Status Wrapper. ---
    uint8_t *csw = rt->msc_csw[idx];
    xhci_bzero(csw, 13);
    if (!xhci_bulk_xfer(m, true, xhci_dma(csw), 13U)) { return false; }
    if (msc_get_le32(csw + 0) != MSC_CSW_SIGNATURE) { return false; }
    if (msc_get_le32(csw + 4) != tag) { return false; }
    return csw[12] == 0U; // bCSWStatus: 0 = command passed
}

// ---- SCSI commands over BOT ----
static bool scsi_test_unit_ready(struct xhci_msc_dev *m) {
    uint8_t cdb[6] = {0}; cdb[0] = SCSI_TEST_UNIT_READY;
    return xhci_msc_bot(m, cdb, 6, false, 0);
}
static void scsi_request_sense(struct xhci_msc_dev *m) {
    uint8_t cdb[6] = {0}; cdb[0] = SCSI_REQUEST_SENSE; cdb[4] = 18;
    xhci_msc_bot(m, cdb, 6, true, 18); // clears a pending CHECK CONDITION
}
static bool scsi_inquiry(struct xhci_msc_dev *m) {
    uint8_t cdb[6] = {0}; cdb[0] = SCSI_INQUIRY; cdb[4] = 36;
    if (!xhci_msc_bot(m, cdb, 6, true, 36)) { return false; }
    uint8_t *d = m->rt->msc_data[m->slot_id - 1U];
    char vp[25]; uint32_t n = 0;                 // vendor(8..15)+product(16..31)
    for (uint32_t i = 8; i < 32U; i++) { char c = (char)d[i]; vp[n++] = (c>=32 && c<127)?c:' '; }
    vp[n] = '\0';
    kprintf("xHCI slot%u: MSC INQUIRY '%s'\n", (unsigned int)m->slot_id, vp);
    return true;
}
static bool scsi_read_capacity10(struct xhci_msc_dev *m, uint64_t *blocks, uint32_t *bsize) {
    uint8_t cdb[10] = {0}; cdb[0] = SCSI_READ_CAPACITY10;
    if (!xhci_msc_bot(m, cdb, 10, true, 8)) { return false; }
    uint8_t *d = m->rt->msc_data[m->slot_id - 1U];
    uint32_t last_lba = msc_get_be32(d + 0);
    uint32_t bs       = msc_get_be32(d + 4);
    if (bs == 0U) { bs = 512U; }
    *blocks = (uint64_t)last_lba + 1U;
    *bsize  = bs;
    return true;
}
// READ(10)/WRITE(10) moving `blocks` sectors at `lba` through the bounce buffer.
static bool scsi_rw10(struct xhci_msc_dev *m, bool write, uint32_t lba, uint16_t blocks) {
    uint32_t bsize = m->blk.block_size;
    uint8_t cdb[10] = {0};
    cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
    cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);  cdb[5] = (uint8_t)lba;      // big-endian LBA
    cdb[7] = (uint8_t)(blocks >> 8); cdb[8] = (uint8_t)blocks; // big-endian block count
    return xhci_msc_bot(m, cdb, 10, !write, (uint32_t)blocks * bsize);
}

// ---- block-device read/write (bounce-buffered, chunked to the bounce size) ----
static int usb_msc_block_read(struct embk_block_device *dev, uint64_t lba,
                              uint32_t count, void *buffer) {
    if (!dev || !buffer) { return -EMBK_EINVAL; }
    struct xhci_msc_dev *m = (struct xhci_msc_dev *)dev->driver_data;
    uint8_t *out = (uint8_t *)buffer;
    uint8_t *bounce = m->rt->msc_data[m->slot_id - 1U];
    uint32_t bsize = dev->block_size;
    uint32_t max_sec = MSC_XFER_BYTES / bsize; if (max_sec == 0U) { max_sec = 1U; }
    int rc = 0;
    // Silence the xHCI IRQ so its handler can't drain the bulk completions this
    // synchronous path busy-polls for; re-enable (draining any queued HID) after.
    xhci_intr_enable(m->rt, false);
    while (count > 0U) {
        uint32_t chunk = (count > max_sec) ? max_sec : count;
        if (!scsi_rw10(m, false, (uint32_t)lba, (uint16_t)chunk)) { rc = -EMBK_EIO; break; }
        memcpy(out, bounce, chunk * bsize);
        out += (size_t)chunk * (size_t)bsize; lba += chunk; count -= chunk;
    }
    xhci_intr_enable(m->rt, true);
    return rc;
}
static int usb_msc_block_write(struct embk_block_device *dev, uint64_t lba,
                               uint32_t count, const void *buffer) {
    if (!dev || !buffer) { return -EMBK_EINVAL; }
    struct xhci_msc_dev *m = (struct xhci_msc_dev *)dev->driver_data;
    const uint8_t *in = (const uint8_t *)buffer;
    uint8_t *bounce = m->rt->msc_data[m->slot_id - 1U];
    uint32_t bsize = dev->block_size;
    uint32_t max_sec = MSC_XFER_BYTES / bsize; if (max_sec == 0U) { max_sec = 1U; }
    int rc = 0;
    xhci_intr_enable(m->rt, false);  // see usb_msc_block_read
    while (count > 0U) {
        uint32_t chunk = (count > max_sec) ? max_sec : count;
        memcpy(bounce, in, (size_t)chunk * (size_t)bsize);
        if (!scsi_rw10(m, true, (uint32_t)lba, (uint16_t)chunk)) { rc = -EMBK_EIO; break; }
        in += (size_t)chunk * (size_t)bsize; lba += chunk; count -= chunk;
    }
    xhci_intr_enable(m->rt, true);
    return rc;
}

// Find the first bulk IN and bulk OUT endpoints in the config-descriptor blob.
static bool xhci_find_bulk_endpoints(struct xhci_runtime_state *rt, uint8_t slot_id,
                                     uint8_t *out_ep, uint8_t *in_ep,
                                     uint16_t *out_mps, uint16_t *in_mps) {
    uint32_t idx = (uint32_t)slot_id - 1U;
    uint8_t *buf = rt->xfr_buf[idx];
    bool have_out = false, have_in = false;
    uint32_t off = 0;
    while (off + 2U <= 64U) {
        uint8_t dlen = buf[off], dtype = buf[off + 1U];
        if (dlen == 0U) { break; }
        if (dtype == USB_DESC_ENDPOINT && off + 7U <= 64U) {
            uint8_t ep_addr = buf[off + 2U];
            uint8_t attrs   = buf[off + 3U];
            uint16_t mps    = (uint16_t)(buf[off + 4U] | ((uint32_t)buf[off + 5U] << 8));
            if ((attrs & 0x03U) == 0x02U) { // bulk transfer type
                if (ep_addr & 0x80U) { if (!have_in)  { *in_ep = ep_addr;  *in_mps = mps;  have_in = true; } }
                else                 { if (!have_out) { *out_ep = ep_addr; *out_mps = mps; have_out = true; } }
            }
        }
        off += dlen;
    }
    return have_in && have_out;
}

// Configure BOTH bulk endpoints (IN and OUT) in a single Configure Endpoint.
static bool xhci_configure_bulk_endpoints(struct xhci_runtime_state *rt,
                                          volatile uint8_t *op,
                                          volatile uint8_t *runtime,
                                          volatile uint8_t *doorbell,
                                          uint8_t slot_id,
                                          uint8_t out_ep, uint8_t in_ep,
                                          uint16_t out_mps, uint16_t in_mps) {
    (void)op;
    uint32_t idx = (uint32_t)slot_id - 1U;
    uint32_t csz = rt->context_size;
    uint32_t out_dci = (uint32_t)(out_ep & 0x0FU) * 2U + 0U; // OUT direction bit 0
    uint32_t in_dci  = (uint32_t)(in_ep  & 0x0FU) * 2U + 1U; // IN  direction bit 1
    uint32_t max_dci = (out_dci > in_dci) ? out_dci : in_dci;

    uint8_t *ictx = rt->input_ctx[idx];
    xhci_bzero(ictx, 1024);
    uint32_t *icc = (uint32_t *)ictx;
    icc[0] = 0;
    icc[1] = (1U << 0U) | (1U << out_dci) | (1U << in_dci); // add slot + both bulk EPs

    // Copy the existing slot context, bump Context Entries to the highest DCI.
    uint8_t *dctx = rt->device_ctx[idx];
    uint32_t *dst_slot = (uint32_t *)(ictx + csz * 1U);
    uint32_t *src_slot = (uint32_t *)dctx;
    for (uint32_t w = 0; w < csz / 4U; w++) { dst_slot[w] = src_slot[w]; }
    dst_slot[0] = (dst_slot[0] & ~(0x1FU << 27)) | (max_dci << 27);

    // OUT ring + endpoint context.
    xhci_bzero(rt->bulk_ring[idx][0], sizeof(rt->bulk_ring[idx][0]));
    rt->bulk_enqueue[idx][0] = 0; rt->bulk_cycle[idx][0] = 1;
    struct xhci_trb *lo = &rt->bulk_ring[idx][0][XHCI_BULK_RING_TRBS - 1U];
    uint64_t out_base = xhci_dma(&rt->bulk_ring[idx][0][0]);
    lo->d0 = (uint32_t)(out_base & 0xFFFFFFFFULL); lo->d1 = (uint32_t)(out_base >> 32);
    lo->d2 = 0; lo->d3 = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC_BIT; // cycle clear; wrap XORs it
    uint32_t *epo = (uint32_t *)(ictx + csz * (out_dci + 1U));
    epo[1] = (XHCI_EP_TYPE_BULK_OUT << 3) | (3U << 1) | ((uint32_t)out_mps << 16); // type + CErr + MPS
    epo[2] = (uint32_t)(out_base & 0xFFFFFFFFULL) | 1U; // TR dequeue ptr, DCS=1
    epo[3] = (uint32_t)(out_base >> 32);
    epo[4] = out_mps; // average TRB length hint

    // IN ring + endpoint context.
    xhci_bzero(rt->bulk_ring[idx][1], sizeof(rt->bulk_ring[idx][1]));
    rt->bulk_enqueue[idx][1] = 0; rt->bulk_cycle[idx][1] = 1;
    struct xhci_trb *li = &rt->bulk_ring[idx][1][XHCI_BULK_RING_TRBS - 1U];
    uint64_t in_base = xhci_dma(&rt->bulk_ring[idx][1][0]);
    li->d0 = (uint32_t)(in_base & 0xFFFFFFFFULL); li->d1 = (uint32_t)(in_base >> 32);
    li->d2 = 0; li->d3 = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC_BIT; // cycle clear; wrap XORs it
    uint32_t *epi = (uint32_t *)(ictx + csz * (in_dci + 1U));
    epi[1] = (XHCI_EP_TYPE_BULK_IN << 3) | (3U << 1) | ((uint32_t)in_mps << 16);
    epi[2] = (uint32_t)(in_base & 0xFFFFFFFFULL) | 1U;
    epi[3] = (uint32_t)(in_base >> 32);
    epi[4] = in_mps;

    rt->bulk_dci[idx][0] = (uint8_t)out_dci;
    rt->bulk_dci[idx][1] = (uint8_t)in_dci;

    // Submit the Configure Endpoint command and wait for completion.
    if (rt->cmd_enqueue >= (XHCI_CMD_RING_TRBS - 1U)) { return false; }
    uint64_t ictx_phys = xhci_dma(ictx);
    struct xhci_trb *cmd = &rt->cmd_ring[rt->cmd_enqueue];
    cmd->d0 = (uint32_t)(ictx_phys & 0xFFFFFFFFULL);
    cmd->d1 = (uint32_t)(ictx_phys >> 32);
    cmd->d2 = 0;
    cmd->d3 = (XHCI_TRB_CFG_ENDPOINT << XHCI_TRB_TYPE_SHIFT)
            | ((uint32_t)slot_id << 24)
            | (rt->cmd_cycle ? XHCI_TRB_CYCLE_BIT : 0U);
    rt->cmd_enqueue++;
    if (rt->cmd_enqueue == (XHCI_CMD_RING_TRBS - 1U)) { rt->cmd_enqueue = 0; rt->cmd_cycle ^= 1U; }
    xhci_ring_doorbell(doorbell, 0, 0);

    uint8_t cc = 0xFF, os = 0;
    if (!xhci_poll_cmd_completion(rt, runtime, 4000000, &cc, &os)) {
        kprintf("xHCI slot%u: bulk Configure Endpoint timeout\n", (unsigned int)slot_id);
        return false;
    }
    kprintf("xHCI slot%u: bulk Configure Endpoint code=%u (out dci=%u in dci=%u)\n",
            (unsigned int)slot_id, (unsigned int)cc,
            (unsigned int)out_dci, (unsigned int)in_dci);
    return cc == 1U;
}

// Orchestrate mass-storage bring-up: configure bulk endpoints, run SCSI probe,
// register the device with the block layer. Called from xhci_dispatch_class.
static bool xhci_msc_setup(struct xhci_runtime_state *rt, uint8_t slot_id,
                           volatile uint8_t *op, volatile uint8_t *runtime,
                           volatile uint8_t *doorbell) {
    uint8_t out_ep = 0, in_ep = 0; uint16_t out_mps = 512, in_mps = 512;
    if (!xhci_find_bulk_endpoints(rt, slot_id, &out_ep, &in_ep, &out_mps, &in_mps)) {
        kprintf("xHCI slot%u: MSC missing bulk IN/OUT endpoints\n", (unsigned int)slot_id);
        return false;
    }
    kprintf("xHCI slot%u: MSC bulk out=0x%x in=0x%x mps=%u/%u\n",
            (unsigned int)slot_id, (unsigned int)out_ep, (unsigned int)in_ep,
            (unsigned int)out_mps, (unsigned int)in_mps);
    if (!xhci_configure_bulk_endpoints(rt, op, runtime, doorbell, slot_id,
                                       out_ep, in_ep, out_mps, in_mps)) {
        return false;
    }

    struct xhci_msc_dev *m = NULL;
    for (uint32_t i = 0; i < (uint32_t)(XHCI_MAX_CONTROLLERS * XHCI_MAX_SLOTS_TRACKED); i++) {
        if (!g_msc_devs[i].used) { m = &g_msc_devs[i]; break; }
    }
    if (!m) { kprintf("xHCI: MSC device table full\n"); return false; }
    m->used = true; m->rt = rt; m->runtime = runtime; m->doorbell = doorbell;
    m->slot_id = slot_id;
    m->blk.block_size = 512; // provisional until READ CAPACITY (scsi_rw10 reads it)
    rt->msc_active[slot_id - 1U] = true;

    // Some devices answer Not Ready at first; nudge with TEST UNIT READY, clearing
    // any CHECK CONDITION via REQUEST SENSE between attempts.
    for (uint32_t t = 0; t < 4U; t++) {
        if (scsi_test_unit_ready(m)) { break; }
        scsi_request_sense(m);
    }
    scsi_inquiry(m);

    uint64_t blocks = 0; uint32_t bsize = 0;
    if (!scsi_read_capacity10(m, &blocks, &bsize)) {
        kprintf("xHCI slot%u: MSC READ CAPACITY failed\n", (unsigned int)slot_id);
        m->used = false; return false;
    }
    kprintf("xHCI slot%u: MSC capacity %u blocks x %u bytes (%u MB)\n",
            (unsigned int)slot_id, (unsigned int)blocks, (unsigned int)bsize,
            (unsigned int)((blocks * bsize) / (1024ULL * 1024ULL)));

    m->blk.name[0] = '\0';               // block layer assigns sdX
    m->blk.block_count = blocks;
    m->blk.block_size  = bsize;
    m->blk.read  = usb_msc_block_read;
    m->blk.write = usb_msc_block_write;
    m->blk.flush = NULL;
    m->blk.driver_data = m;
    m->blk.dma_max_phys = 0xFFFFFFFFFFFFFFFFULL; // DMA goes through kernel-BSS bounce
    m->blk.needs_kernel_range = true;
    embk_block_register(&m->blk);
    return true;
}

// Count connected root ports for diagnostics. This MUST be read-only: issuing a
// port reset (PR) here would re-reset the port we just enumerated on, throwing
// the device back to the Default state and undoing SET_ADDRESS/SET_CONFIGURATION
// — after which the configured HID endpoint stops delivering reports. Any port
// reset needed for enumeration is the controller's/QEMU's job on connect.
static uint32_t xhci_probe_ports(volatile uint8_t *op, uint32_t max_ports) {
    uint32_t present = 0;
    uint32_t ports_to_probe = (max_ports > 16U) ? 16U : max_ports;

    for (uint32_t p = 0; p < ports_to_probe; p++) {
        uint32_t reg = XHCI_PORTSC_BASE + (p * XHCI_PORT_STRIDE);
        uint32_t portsc = xhci_read32(op, reg);

        if (!(portsc & XHCI_PORTSC_CCS)) {
            continue;
        }

        uint32_t speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
        bool enabled = (portsc & XHCI_PORTSC_PED) != 0;
        kprintf("xHCI:   port%u connected (PORTSC=%x speed=%u enabled=%u)\n",
                (unsigned int)(p + 1U),
                (unsigned int)portsc,
                (unsigned int)speed,
                enabled ? 1U : 0U);

        present++;
    }

    return present;
}

bool xhci_init_controller(struct usb_controller *ctrl) {
    if (!ctrl || !ctrl->bar0.valid || !ctrl->bar0.is_mmio) {
        return false;
    }

    // Some firmware leaves BAR size probing ambiguous; use a safe minimum.
    uint64_t map_size = ctrl->bar0.size ? ctrl->bar0.size : 0x4000;
    uint64_t virt = vmm_map_mmio(ctrl->bar0.address, map_size);
    if (!virt) {
        kprintf("xHCI: MMIO map failed for %x:%x.%x\n",
                (unsigned int)ctrl->pci.bus,
                (unsigned int)ctrl->pci.device,
                (unsigned int)ctrl->pci.function);
        return false;
    }

    ctrl->mmio_virt = virt;
    ctrl->mmio_size = map_size;
    volatile uint8_t *mmio = (volatile uint8_t *)(uintptr_t)virt;

    uint8_t caplen = xhci_read8(mmio, XHCI_CAPLENGTH);
    uint16_t hci_vers = xhci_read16(mmio, XHCI_HCIVERSION);
    uint32_t hcsparams1 = xhci_read32(mmio, XHCI_HCSPARAMS1);
    uint32_t hccparams1 = xhci_read32(mmio, XHCI_HCCPARAMS1);
    uint32_t db_offset = xhci_read32(mmio, XHCI_DBOFF) & ~0x3U;
    uint32_t rt_offset = xhci_read32(mmio, XHCI_RTSOFF) & ~0x1FU;

    uint32_t max_slots = hcsparams1 & 0xFF;
    uint32_t max_ports = (hcsparams1 >> 24) & 0xFF;
    uint32_t context_size = (hccparams1 & (1U << 2)) ? 64U : 32U;

    // xHCI register blocks are discovered via offsets from capability space.
    volatile uint8_t *op = mmio + caplen;
    volatile uint8_t *runtime = mmio + rt_offset;
    volatile uint8_t *doorbell = mmio + db_offset;
    uint32_t usbcmd = xhci_read32(op, XHCI_OP_USBCMD);
    uint32_t usbsts = xhci_read32(op, XHCI_OP_USBSTS);
    uint32_t pagesize = xhci_read32(op, XHCI_OP_PAGESIZE);
    uint32_t config = xhci_read32(op, XHCI_OP_CONFIG);

    kprintf("xHCI: %x:%x.%x CAPL=%u VER=%x SLOTS=%u PORTS=%u CMD=%x STS=%x PGSZ=%x CFG=%x\n",
            (unsigned int)ctrl->pci.bus,
            (unsigned int)ctrl->pci.device,
            (unsigned int)ctrl->pci.function,
            (unsigned int)caplen,
            (unsigned int)hci_vers,
            (unsigned int)max_slots,
            (unsigned int)max_ports,
            (unsigned int)usbcmd,
            (unsigned int)usbsts,
            (unsigned int)pagesize,
            (unsigned int)config);

    if ((pagesize & 0x1U) == 0) {
        kprintf("xHCI: controller does not advertise 4K page support\n");
        return false;
    }

    if (!xhci_controller_reset_and_run(op)) {
        return false;
    }

    struct xhci_runtime_state *rt = xhci_get_runtime_state(ctrl);
    if (!rt) {
        kprintf("xHCI: no runtime slots available\n");
        return false;
    }

    // Remember the register blocks so the IRQ handler and runtime poll can reach
    // them after init returns.
    rt->op_regs       = op;
    rt->runtime_regs  = runtime;
    rt->doorbell_regs = doorbell;

    rt->context_size = (uint8_t)context_size;
    rt->tracked_slots = (uint8_t)((max_slots < XHCI_MAX_SLOTS_TRACKED) ?
                                  max_slots : XHCI_MAX_SLOTS_TRACKED);

    if (!xhci_setup_rings(rt, op, runtime)) {
        kprintf("xHCI: ring setup failed\n");
        return false;
    }

    // Program Max Device Slots Enabled (CONFIG[7:0]).
    uint32_t cfg = xhci_read32(op, XHCI_OP_CONFIG);
    cfg &= ~0xFFU;
    cfg |= (max_slots > 8U) ? 8U : (max_slots ? max_slots : 1U);
    xhci_write32(op, XHCI_OP_CONFIG, cfg);

    // First real command path: Enable Slot. This validates command/event rings.
    if (xhci_submit_enable_slot(rt, doorbell)) {
        uint8_t cc = 0xFF;
        uint8_t slot = 0;
        if (xhci_poll_cmd_completion(rt, runtime, 4000000, &cc, &slot)) {
            kprintf("xHCI: Enable Slot completion code=%u slot=%u\n",
                    (unsigned int)cc, (unsigned int)slot);

            // Address the first connected root port as the first real device flow.
            uint32_t root_port = xhci_find_first_connected_port(op, max_ports);
            if (cc == 1U && slot != 0U && root_port != 0U) {
                uint8_t speed = xhci_port_speed(op, root_port - 1U);
                if (!xhci_prepare_address_device_context(rt, slot,
                                                         (uint8_t)root_port,
                                                         speed)) {
                    kprintf("xHCI: failed to prepare input context for slot %u\n",
                            (unsigned int)slot);
                } else {
                    uint64_t ictx = xhci_dma(rt->input_ctx[(uint32_t)slot - 1U]);
                    if (xhci_submit_address_device(rt, doorbell, slot, ictx)) {
                        uint8_t cc2 = 0xFF;
                        uint8_t slot2 = 0;
                        if (xhci_poll_cmd_completion(rt, runtime, 4000000,
                                                     &cc2, &slot2)) {
                            kprintf("xHCI: Address Device completion code=%u slot=%u port=%u speed=%u\n",
                                    (unsigned int)cc2,
                                    (unsigned int)slot2,
                                    (unsigned int)root_port,
                                    (unsigned int)speed);

                            // Address Device succeeded (cc2==1) — the slot now
                            // has a real USB address and EP0 is live.  Fetch the
                            // standard 18-byte Device Descriptor as the first
                            // real data transfer to confirm end-to-end function.
                            if (cc2 == 1U) {
                                uint32_t sidx = (uint32_t)slot - 1U;
                                // Initialise the EP0 enqueue/cycle for this slot.
                                rt->ep0_enqueue[sidx] = 0;
                                rt->ep0_cycle[sidx]   = 1;

                                // Step 1: GET_DESCRIPTOR(Device) — confirm EP0 works.
                                if (xhci_get_device_descriptor(rt, runtime,
                                                               doorbell, slot)) {
                                    // Step 2: SET_ADDRESS — assign a stable device addr.
                                    // Use slot_id as the USB address (legal range 1-127).
                                    xhci_set_address(rt, runtime, doorbell,
                                                     slot, slot);

                                    // Step 3: GET_DESCRIPTOR(Configuration) — fetch the
                                    // full config blob so we can identify the class.
                                    if (xhci_get_config_descriptor(rt, runtime,
                                                                   doorbell, slot)) {
                                        // Step 4: SET_CONFIGURATION — activate the
                                        // interfaces/endpoints (bConfigurationValue is
                                        // byte 5 of the configuration descriptor). The
                                        // device only produces HID reports once this
                                        // moves it into the Configured state.
                                        uint8_t cfg_val =
                                            rt->xfr_buf[(uint32_t)slot - 1U][5];
                                        xhci_set_configuration(rt, runtime, doorbell,
                                                               slot, cfg_val);

                                        // Step 5: Walk descriptors and dispatch by class.
                                        xhci_dispatch_class(rt, slot, op,
                                                            runtime, doorbell);
                                    }
                                }
                            }
                        } else {
                            kprintf("xHCI: Address Device completion timeout\n");
                        }
                    } else {
                        kprintf("xHCI: failed to submit Address Device command\n");
                    }
                }
            }
        } else {
            kprintf("xHCI: Enable Slot completion timeout\n");
        }
    } else {
        kprintf("xHCI: failed to submit Enable Slot command\n");
    }

    ctrl->max_ports = (uint8_t)max_ports;
    ctrl->devices_present = (uint8_t)xhci_probe_ports(op, max_ports);
    kprintf("xHCI: detected %u connected device(s) on %u port(s)\n",
            (unsigned int)ctrl->devices_present,
            (unsigned int)ctrl->max_ports);

    return true;
}
