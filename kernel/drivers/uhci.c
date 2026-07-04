// UHCI (Universal Host Controller Interface, USB 1.0/1.1) driver.
//
// UHCI is fully port-I/O programmed; only the schedule (frame list, queue
// heads, transfer descriptors) lives in memory and is DMA'd by the HC.
// Schedule layout built at init and kept forever:
//
//   frame list (1024 entries, all identical)
//        └─> intr QH[0] ─> intr QH[1] ─> ... ─> work QH ─> terminate
//
// Interrupt endpoints each own an intr QH so a NAKing keyboard can't block
// another device. Control/bulk transfers are run synchronously through the
// work QH and torn down again.

#include "uhci.h"
#include "usb_core.h"

#include "../include/io.h"
#include "../include/kprintf.h"
#include "../include/kstring.h"
#include "../mm/pmm.h"
#include "pit.h"

// I/O registers (offsets from BAR4 I/O base).
#define UHCI_USBCMD    0x00
#define UHCI_USBSTS    0x02
#define UHCI_USBINTR   0x04
#define UHCI_FRNUM     0x06
#define UHCI_FRBASEADD 0x08
#define UHCI_SOFMOD    0x0C
#define UHCI_PORTSC1   0x10

#define UHCI_CMD_RS      (1 << 0)
#define UHCI_CMD_HCRESET (1 << 1)
#define UHCI_CMD_GRESET  (1 << 2)
#define UHCI_CMD_CF      (1 << 6)
#define UHCI_CMD_MAXP    (1 << 7)

#define UHCI_PORT_CCS  (1 << 0)   // current connect status
#define UHCI_PORT_CSC  (1 << 1)   // connect status change (W1C)
#define UHCI_PORT_PED  (1 << 2)   // port enabled
#define UHCI_PORT_PEDC (1 << 3)   // enable change (W1C)
#define UHCI_PORT_LSDA (1 << 8)   // low-speed device attached
#define UHCI_PORT_PR   (1 << 9)   // port reset

// TD control/status word.
#define UHCI_TD_ACTLEN_MASK 0x7FF
#define UHCI_TD_STALLED   (1 << 22)
#define UHCI_TD_NAK       (1 << 19)
#define UHCI_TD_CRC_TO    (1 << 18)
#define UHCI_TD_BITSTUFF  (1 << 17)
#define UHCI_TD_ANY_ERR   (UHCI_TD_STALLED | UHCI_TD_CRC_TO | UHCI_TD_BITSTUFF | (1 << 21) | (1 << 20))
#define UHCI_TD_ACTIVE    (1 << 23)
#define UHCI_TD_IOC       (1 << 24)
#define UHCI_TD_LS        (1 << 26)
#define UHCI_TD_CERR_3    (3 << 27)

// TD token PIDs.
#define UHCI_PID_IN    0x69
#define UHCI_PID_OUT   0xE1
#define UHCI_PID_SETUP 0x2D

// Link pointer bits.
#define UHCI_LINK_TERM 0x1
#define UHCI_LINK_QH   0x2
#define UHCI_LINK_VF   0x4    // depth-first within a queue

#define UHCI_MAX_HC     4
#define UHCI_NUM_TD     80    // enough for a 4 KB bulk chunk at MPS 64
#define UHCI_NUM_INTR   4     // concurrently polled interrupt endpoints

struct uhci_td {
    uint32_t link;
    uint32_t cs;
    uint32_t token;
    uint32_t buffer;
} __attribute__((aligned(16)));

struct uhci_qh {
    uint32_t head;      // horizontal link
    uint32_t element;   // vertical link (first TD)
    uint32_t pad[2];    // keep 16-byte size/alignment
} __attribute__((aligned(16)));

struct uhci_intr_slot {
    bool active;                 // TD armed
    struct usb_device *dev;
    uint8_t ep_addr;
    uint16_t len;
    struct uhci_td td;
    uint8_t buf[64] __attribute__((aligned(16)));
};

struct uhci_hc {
    bool used;
    uint16_t io;

    uint32_t framelist[1024] __attribute__((aligned(4096)));
    struct uhci_qh qh_intr[UHCI_NUM_INTR];
    struct uhci_qh qh_work;
    struct uhci_td td[UHCI_NUM_TD];
    uint8_t setup_buf[8]  __attribute__((aligned(16)));
    uint8_t bounce[4096]  __attribute__((aligned(64)));

    struct uhci_intr_slot intr[UHCI_NUM_INTR];
};

static struct uhci_hc g_uhci[UHCI_MAX_HC];

static inline uint64_t uhci_dma(const volatile void *p) {
    return KV2P((uint64_t)(uintptr_t)p);
}

static inline uint16_t usb_ep_mps_of(struct usb_device *dev, uint8_t ep_addr) {
    for (uint32_t i = 0; i < dev->num_eps; i++) {
        if (dev->eps[i].addr == ep_addr) return dev->eps[i].mps;
    }
    return (dev->speed == USB_SPEED_LOW) ? 8 : 64;
}

static void uhci_fill_td(struct uhci_td *td, uint32_t link,
                         bool low_speed, uint8_t pid, uint8_t addr,
                         uint8_t ep, uint8_t toggle,
                         uint32_t len, uint32_t buf_phys) {
    uint32_t maxlen = (len == 0) ? 0x7FF : ((len - 1) & 0x7FF);
    td->link = link;
    td->cs = UHCI_TD_ACTIVE | UHCI_TD_CERR_3 | (low_speed ? UHCI_TD_LS : 0);
    td->token = ((uint32_t)maxlen << 21) | ((uint32_t)(toggle & 1) << 19) |
                ((uint32_t)(ep & 0x0F) << 15) | ((uint32_t)(addr & 0x7F) << 8) |
                pid;
    td->buffer = buf_phys;
}

// Run a TD chain through the work QH; returns when the last TD retires,
// errors, or the timeout expires. Returns index of first errored TD, -1 on
// clean completion, -2 on timeout.
static int uhci_run_chain(struct uhci_hc *hc, struct uhci_td *first,
                          struct uhci_td *last, uint32_t timeout_ms) {
    hc->qh_work.element = (uint32_t)uhci_dma(first);

    // ~1 poll per microsecond-ish; scaled by timeout_ms.
    uint64_t spins = (uint64_t)timeout_ms * 20000;
    for (uint64_t i = 0; i < spins; i++) {
        uint32_t cs = ((volatile struct uhci_td *)last)->cs;
        if (!(cs & UHCI_TD_ACTIVE)) {
            hc->qh_work.element = UHCI_LINK_TERM;
            return -1;
        }
        // Scan for a halted TD earlier in the chain (stall/error stops the
        // queue without retiring the final TD).
        for (struct uhci_td *t = first; ; t++) {
            uint32_t tcs = ((volatile struct uhci_td *)t)->cs;
            if (!(tcs & UHCI_TD_ACTIVE) && (tcs & UHCI_TD_ANY_ERR)) {
                hc->qh_work.element = UHCI_LINK_TERM;
                return (int)(t - first);
            }
            if (t == last) break;
        }
    }
    hc->qh_work.element = UHCI_LINK_TERM;
    return -2;
}

// ---------------------------------------------------------------------------
// usb_hcd_ops
// ---------------------------------------------------------------------------

static int uhci_control(struct usb_device *dev,
                        const struct usb_setup_packet *setup, void *data) {
    struct uhci_hc *hc = (struct uhci_hc *)dev->hc;
    bool ls = (dev->speed == USB_SPEED_LOW);
    bool dir_in = (setup->bmRequestType & 0x80) != 0;
    uint32_t wlen = setup->wLength;
    uint16_t mps = dev->ep0_mps ? dev->ep0_mps : 8;

    if (wlen > sizeof(hc->bounce)) return USB_ERR_IO;
    memcpy(hc->setup_buf, setup, 8);
    if (wlen && !dir_in && data) memcpy(hc->bounce, data, wlen);

    // SETUP + data packets + STATUS.
    uint32_t ntd = 2 + (wlen + mps - 1) / mps;
    if (ntd > UHCI_NUM_TD) return USB_ERR_IO;

    uint32_t idx = 0;
    struct uhci_td *tds = hc->td;

    uhci_fill_td(&tds[idx], 0, ls, UHCI_PID_SETUP, dev->addr, 0, 0,
                 8, (uint32_t)uhci_dma(hc->setup_buf));
    idx++;

    uint8_t toggle = 1;
    uint32_t off = 0;
    while (off < wlen) {
        uint32_t chunk = wlen - off;
        if (chunk > mps) chunk = mps;
        uhci_fill_td(&tds[idx], 0, ls,
                     dir_in ? UHCI_PID_IN : UHCI_PID_OUT,
                     dev->addr, 0, toggle, chunk,
                     (uint32_t)uhci_dma(hc->bounce + off));
        toggle ^= 1;
        off += chunk;
        idx++;
    }

    // Status stage: OUT only after an IN data stage; IN otherwise (including
    // the no-data case, where the status handshake is always IN). DATA1.
    uhci_fill_td(&tds[idx], 0, ls,
                 (dir_in && wlen) ? UHCI_PID_OUT : UHCI_PID_IN,
                 dev->addr, 0, 1, 0, 0);
    tds[idx].cs |= UHCI_TD_IOC;

    // Chain: depth-first links so the whole transfer runs in one frame pass.
    for (uint32_t i = 0; i < idx; i++) {
        tds[i].link = (uint32_t)uhci_dma(&tds[i + 1]) | UHCI_LINK_VF;
    }
    tds[idx].link = UHCI_LINK_TERM;

    int rc = uhci_run_chain(hc, &tds[0], &tds[idx], 500);
    if (rc == -2) return USB_ERR_TIMEOUT;
    if (rc >= 0) {
        return (tds[rc].cs & UHCI_TD_STALLED) ? USB_ERR_STALL : USB_ERR_IO;
    }

    // Sum the data-stage actual lengths.
    uint32_t got = 0;
    for (uint32_t i = 1; i < idx; i++) {
        got += (tds[i].cs + 1) & UHCI_TD_ACTLEN_MASK;
    }
    if (dir_in && data && got) {
        memcpy(data, hc->bounce, got > wlen ? wlen : got);
    }
    return (int)got;
}

static int uhci_bulk(struct usb_device *dev, uint8_t ep_addr,
                     void *data, uint32_t len) {
    struct uhci_hc *hc = (struct uhci_hc *)dev->hc;
    bool ls = (dev->speed == USB_SPEED_LOW);
    bool dir_in = (ep_addr & 0x80) != 0;
    uint16_t mps = usb_ep_mps_of(dev, ep_addr);

    if (len > sizeof(hc->bounce)) return USB_ERR_IO;
    if (!dir_in && data && len) memcpy(hc->bounce, data, len);

    uint32_t npkts = len ? (len + mps - 1) / mps : 1;
    if (npkts > UHCI_NUM_TD) return USB_ERR_IO;

    uint8_t toggle = usb_toggle_get(dev, ep_addr);
    struct uhci_td *tds = hc->td;
    uint32_t idx = 0, off = 0;

    do {
        uint32_t chunk = len - off;
        if (chunk > mps) chunk = mps;
        uhci_fill_td(&tds[idx], 0, ls,
                     dir_in ? UHCI_PID_IN : UHCI_PID_OUT,
                     dev->addr, ep_addr & 0x0F, toggle, chunk,
                     chunk ? (uint32_t)uhci_dma(hc->bounce + off) : 0);
        toggle ^= 1;
        off += chunk;
        idx++;
    } while (off < len);

    for (uint32_t i = 0; i + 1 < idx; i++) {
        tds[i].link = (uint32_t)uhci_dma(&tds[i + 1]) | UHCI_LINK_VF;
    }
    tds[idx - 1].link = UHCI_LINK_TERM;
    tds[idx - 1].cs |= UHCI_TD_IOC;

    int rc = uhci_run_chain(hc, &tds[0], &tds[idx - 1], 2000);
    if (rc == -2) return USB_ERR_TIMEOUT;
    if (rc >= 0) {
        // Completed packets before the error still advanced the toggle.
        usb_toggle_set(dev, ep_addr, (usb_toggle_get(dev, ep_addr) ^ (rc & 1)));
        return (tds[rc].cs & UHCI_TD_STALLED) ? USB_ERR_STALL : USB_ERR_IO;
    }

    usb_toggle_set(dev, ep_addr, toggle);

    uint32_t got = 0;
    for (uint32_t i = 0; i < idx; i++) {
        got += (tds[i].cs + 1) & UHCI_TD_ACTLEN_MASK;
    }
    if (got > len) got = len;
    if (dir_in && data && got) memcpy(data, hc->bounce, got);
    return (int)got;
}

static struct uhci_intr_slot *uhci_find_slot(struct uhci_hc *hc,
                                             struct usb_device *dev,
                                             uint8_t ep_addr, bool alloc) {
    for (uint32_t i = 0; i < UHCI_NUM_INTR; i++) {
        if (hc->intr[i].dev == dev && hc->intr[i].ep_addr == ep_addr) {
            return &hc->intr[i];
        }
    }
    if (!alloc) return NULL;
    for (uint32_t i = 0; i < UHCI_NUM_INTR; i++) {
        if (!hc->intr[i].dev) {
            hc->intr[i].dev = dev;
            hc->intr[i].ep_addr = ep_addr;
            return &hc->intr[i];
        }
    }
    return NULL;
}

static int uhci_int_submit(struct usb_device *dev, uint8_t ep_addr,
                           uint32_t len) {
    struct uhci_hc *hc = (struct uhci_hc *)dev->hc;
    struct uhci_intr_slot *slot = uhci_find_slot(hc, dev, ep_addr, true);
    if (!slot || len > sizeof(slot->buf)) return USB_ERR_IO;
    if (slot->active) return 0;   // already armed

    slot->len = (uint16_t)len;
    uhci_fill_td(&slot->td, UHCI_LINK_TERM,
                 dev->speed == USB_SPEED_LOW, UHCI_PID_IN,
                 dev->addr, ep_addr & 0x0F, usb_toggle_get(dev, ep_addr),
                 len, (uint32_t)uhci_dma(slot->buf));
    __sync_synchronize();

    uint32_t qi = (uint32_t)(slot - hc->intr);
    hc->qh_intr[qi].element = (uint32_t)uhci_dma(&slot->td);
    slot->active = true;
    return 0;
}

static int uhci_int_poll(struct usb_device *dev, uint8_t ep_addr,
                         void *buf, uint32_t len) {
    struct uhci_hc *hc = (struct uhci_hc *)dev->hc;
    struct uhci_intr_slot *slot = uhci_find_slot(hc, dev, ep_addr, false);
    if (!slot || !slot->active) return USB_ERR_IO;

    uint32_t cs = ((volatile struct uhci_td *)&slot->td)->cs;
    if (cs & UHCI_TD_ACTIVE) return USB_ERR_PENDING;

    uint32_t qi = (uint32_t)(slot - hc->intr);
    hc->qh_intr[qi].element = UHCI_LINK_TERM;
    slot->active = false;

    if (cs & UHCI_TD_ANY_ERR) {
        return (cs & UHCI_TD_STALLED) ? USB_ERR_STALL : USB_ERR_IO;
    }

    usb_toggle_set(dev, ep_addr, usb_toggle_get(dev, ep_addr) ^ 1);
    uint32_t got = (cs + 1) & UHCI_TD_ACTLEN_MASK;
    if (got > len) got = len;
    if (got) memcpy(buf, slot->buf, got);
    return (int)got;
}

static const struct usb_hcd_ops g_uhci_ops = {
    .control = uhci_control,
    .bulk = uhci_bulk,
    .int_submit = uhci_int_submit,
    .int_poll = uhci_int_poll,
};

// ---------------------------------------------------------------------------
// Controller bring-up
// ---------------------------------------------------------------------------

static void uhci_port_scan(struct uhci_hc *hc) {
    for (uint32_t p = 0; p < 2; p++) {
        uint16_t reg = (uint16_t)(hc->io + UHCI_PORTSC1 + p * 2);
        uint16_t sc = inw(reg);
        if (!(sc & UHCI_PORT_CCS)) continue;

        // Reset the port, then enable it.
        outw(reg, UHCI_PORT_PR);
        pit_delay_ms(50);
        outw(reg, 0);
        pit_delay_ms(10);
        outw(reg, UHCI_PORT_PED | UHCI_PORT_CSC | UHCI_PORT_PEDC);
        pit_delay_ms(10);

        sc = inw(reg);
        if (!(sc & UHCI_PORT_PED)) {
            kprintf("UHCI: port %u failed to enable (sc=%x)\n",
                    (unsigned int)(p + 1), (unsigned int)sc);
            continue;
        }

        uint8_t speed = (sc & UHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
        kprintf("UHCI: port %u connected (%s speed)\n",
                (unsigned int)(p + 1),
                speed == USB_SPEED_LOW ? "low" : "full");

        struct usb_device *dev = usb_alloc_device(&g_uhci_ops, hc, NULL, speed);
        if (!dev) return;
        if (usb_enumerate(dev) != 0) {
            usb_free_device(dev);
        }
    }
}

bool uhci_init_controller(struct usb_controller *ctrl) {
    // UHCI's I/O registers live in BAR4.
    struct pci_bar bar = pci_read_bar(ctrl->pci.bus, ctrl->pci.device,
                                      ctrl->pci.function, 4);
    if (!bar.valid || bar.is_mmio) {
        kprintf("UHCI: no usable I/O BAR4\n");
        return false;
    }

    struct uhci_hc *hc = NULL;
    for (uint32_t i = 0; i < UHCI_MAX_HC; i++) {
        if (!g_uhci[i].used) { hc = &g_uhci[i]; break; }
    }
    if (!hc) {
        kprintf("UHCI: controller table full\n");
        return false;
    }
    memset(hc, 0, sizeof(*hc));
    hc->used = true;
    hc->io = (uint16_t)bar.address;

    // Take the controller away from the BIOS (legacy support register).
    pci_write16(ctrl->pci.bus, ctrl->pci.device, ctrl->pci.function,
                0xC0, 0x8F00);

    // Global reset, then HC reset.
    outw(hc->io + UHCI_USBCMD, UHCI_CMD_GRESET);
    pit_delay_ms(20);
    outw(hc->io + UHCI_USBCMD, 0);
    pit_delay_ms(10);
    outw(hc->io + UHCI_USBCMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 1000 && (inw(hc->io + UHCI_USBCMD) & UHCI_CMD_HCRESET); i++) {
        pit_delay_ms(1);
    }

    // Build the permanent schedule.
    for (uint32_t i = 0; i < UHCI_NUM_INTR; i++) {
        hc->qh_intr[i].head = (i + 1 < UHCI_NUM_INTR)
            ? (uint32_t)uhci_dma(&hc->qh_intr[i + 1]) | UHCI_LINK_QH
            : (uint32_t)uhci_dma(&hc->qh_work) | UHCI_LINK_QH;
        hc->qh_intr[i].element = UHCI_LINK_TERM;
    }
    hc->qh_work.head = UHCI_LINK_TERM;
    hc->qh_work.element = UHCI_LINK_TERM;
    uint32_t first = (uint32_t)uhci_dma(&hc->qh_intr[0]) | UHCI_LINK_QH;
    for (uint32_t i = 0; i < 1024; i++) {
        hc->framelist[i] = first;
    }

    outl(hc->io + UHCI_FRBASEADD, (uint32_t)uhci_dma(hc->framelist));
    outw(hc->io + UHCI_FRNUM, 0);
    outb(hc->io + UHCI_SOFMOD, 64);
    outw(hc->io + UHCI_USBINTR, 0);   // polled operation
    outw(hc->io + UHCI_USBSTS, 0x3F); // clear stale status
    outw(hc->io + UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);

    kprintf("UHCI: controller at io 0x%x running\n", (unsigned int)hc->io);
    ctrl->max_ports = 2;

    uhci_port_scan(hc);
    return true;
}
