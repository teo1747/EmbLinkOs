// EHCI (Enhanced Host Controller Interface, USB 2.0) driver.
//
// High-speed transfers run through the async schedule: a permanent head QH
// (H=1) with transfer QHs linked in synchronously per transfer. Interrupt-IN
// endpoints get a persistent QH on the periodic frame list (S-mask 0x01) and
// are polled from usb_core_poll().
//
// Full/low-speed devices are not driven through split transactions; instead
// the port-owner bit hands them to a companion UHCI/OHCI controller, which is
// why usb_init() brings EHCI up first.

#include "ehci.h"
#include "usb_core.h"

#include "../include/kprintf.h"
#include "../include/kstring.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "pit.h"

// Capability registers.
#define EHCI_CAPLENGTH  0x00
#define EHCI_HCSPARAMS  0x04
#define EHCI_HCCPARAMS  0x08

// Operational registers (offset from op base).
#define EHCI_USBCMD          0x00
#define EHCI_USBSTS          0x04
#define EHCI_USBINTR         0x08
#define EHCI_FRINDEX         0x0C
#define EHCI_CTRLDSSEGMENT   0x10
#define EHCI_PERIODICLISTBASE 0x14
#define EHCI_ASYNCLISTADDR   0x18
#define EHCI_CONFIGFLAG      0x40
#define EHCI_PORTSC          0x44

#define EHCI_CMD_RS      (1 << 0)
#define EHCI_CMD_HCRESET (1 << 1)
#define EHCI_CMD_PSE     (1 << 4)
#define EHCI_CMD_ASE     (1 << 5)
#define EHCI_CMD_IAAD    (1 << 6)
#define EHCI_CMD_ITC8    (8 << 16)

#define EHCI_STS_IAA     (1 << 5)
#define EHCI_STS_HCH     (1 << 12)

#define EHCI_PORT_CCS   (1 << 0)
#define EHCI_PORT_CSC   (1 << 1)
#define EHCI_PORT_PED   (1 << 2)
#define EHCI_PORT_PEDC  (1 << 3)
#define EHCI_PORT_OCC   (1 << 5)
#define EHCI_PORT_PR    (1 << 8)
#define EHCI_PORT_LINE_MASK (3 << 10)
#define EHCI_PORT_LINE_K    (1 << 10)   // K-state: low-speed device
#define EHCI_PORT_PP    (1 << 12)
#define EHCI_PORT_OWNER (1 << 13)
#define EHCI_PORT_W1C   (EHCI_PORT_CSC | EHCI_PORT_PEDC | EHCI_PORT_OCC)

// Link pointer encodings.
#define EHCI_LINK_TERM 1
#define EHCI_LINK_QH   (1 << 1)

// qTD token.
#define QTD_STS_ACTIVE   (1 << 7)
#define QTD_STS_HALTED   (1 << 6)
#define QTD_STS_BABBLE   (1 << 4)
#define QTD_STS_XACTERR  (1 << 3)
#define QTD_PID_OUT      (0U << 8)
#define QTD_PID_IN       (1U << 8)
#define QTD_PID_SETUP    (2U << 8)
#define QTD_CERR_3       (3U << 10)
#define QTD_IOC          (1U << 15)
#define QTD_LEN_SHIFT    16
#define QTD_DT           (1U << 31)

// QH endpoint characteristics.
#define QH_EPS_HIGH  (2U << 12)
#define QH_DTC       (1U << 14)
#define QH_HEAD      (1U << 15)
#define QH_MULT_1    (1U << 30)

#define EHCI_MAX_HC   2
#define EHCI_NUM_QTD  8
#define EHCI_NUM_INTR 4

struct ehci_qtd {
    uint32_t next;
    uint32_t altnext;
    uint32_t token;
    uint32_t buf[5];
} __attribute__((aligned(32)));

struct ehci_qh {
    uint32_t hlink;
    uint32_t epchar;
    uint32_t epcap;
    uint32_t current;
    // Transfer overlay
    uint32_t ov_next;
    uint32_t ov_altnext;
    uint32_t ov_token;
    uint32_t ov_buf[5];
    uint32_t pad[4];
} __attribute__((aligned(64)));

struct ehci_intr_slot {
    bool active;
    struct usb_device *dev;
    uint8_t ep_addr;
    uint16_t len;
    struct ehci_qtd qtd;
    uint8_t buf[64] __attribute__((aligned(64)));
};

struct ehci_hc {
    bool used;
    volatile uint8_t *cap;
    volatile uint8_t *op;
    uint32_t nports;

    uint32_t framelist[1024] __attribute__((aligned(4096)));
    struct ehci_qh qh_head;                    // async list head (H=1)
    struct ehci_qh qh_work;                    // per-transfer QH
    struct ehci_qh qh_intr[EHCI_NUM_INTR];     // periodic interrupt QHs
    struct ehci_qtd qtd[EHCI_NUM_QTD];
    uint8_t setup_buf[8] __attribute__((aligned(32)));
    uint8_t bounce[4096] __attribute__((aligned(4096)));

    struct ehci_intr_slot intr[EHCI_NUM_INTR];
};

static struct ehci_hc g_ehci[EHCI_MAX_HC];

static inline uint32_t ehci_dma(const volatile void *p) {
    return (uint32_t)KV2P((uint64_t)(uintptr_t)p);
}

static inline uint32_t ehci_op_read(struct ehci_hc *hc, uint32_t off) {
    return *(volatile uint32_t *)(hc->op + off);
}

static inline void ehci_op_write(struct ehci_hc *hc, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(hc->op + off) = v;
}

static inline uint16_t ehci_ep_mps(struct usb_device *dev, uint8_t ep_addr) {
    for (uint32_t i = 0; i < dev->num_eps; i++) {
        if (dev->eps[i].addr == ep_addr) return dev->eps[i].mps;
    }
    return 512;
}

static void ehci_fill_qtd(struct ehci_qtd *q, uint32_t next_phys,
                          uint32_t pid, uint8_t toggle, const void *buf,
                          uint32_t len, bool ioc) {
    q->next = next_phys;
    q->altnext = EHCI_LINK_TERM;
    q->token = (len << QTD_LEN_SHIFT) | (toggle ? QTD_DT : 0) |
               QTD_CERR_3 | pid | QTD_STS_ACTIVE | (ioc ? QTD_IOC : 0);
    if (buf && len) {
        uint32_t phys = ehci_dma(buf);
        q->buf[0] = phys;
        for (int i = 1; i < 5; i++) {
            q->buf[i] = (phys & ~0xFFFU) + 4096U * i;
        }
    } else {
        for (int i = 0; i < 5; i++) q->buf[i] = 0;
    }
}

// Link qh_work behind the async head, run the qTD chain, unlink, handshake
// the doorbell. Returns -1 success, -2 timeout, index of failed qTD else.
static int ehci_run_async(struct ehci_hc *hc, uint32_t nqtd,
                          uint32_t timeout_ms) {
    struct ehci_qh *qh = &hc->qh_work;
    volatile struct ehci_qtd *last =
        (volatile struct ehci_qtd *)&hc->qtd[nqtd - 1];

    qh->current = 0;
    qh->ov_next = ehci_dma(&hc->qtd[0]);
    qh->ov_altnext = EHCI_LINK_TERM;
    qh->ov_token = 0;
    for (int i = 0; i < 5; i++) qh->ov_buf[i] = 0;

    // Insert after the head.
    qh->hlink = hc->qh_head.hlink;
    __sync_synchronize();
    hc->qh_head.hlink = ehci_dma(qh) | EHCI_LINK_QH;

    int result = -2;
    uint64_t spins = (uint64_t)timeout_ms * 20000;
    for (uint64_t i = 0; i < spins; i++) {
        uint32_t tok = last->token;
        if (!(tok & QTD_STS_ACTIVE)) {
            result = -1;
            break;
        }
        for (uint32_t t = 0; t < nqtd; t++) {
            uint32_t tt = ((volatile struct ehci_qtd *)&hc->qtd[t])->token;
            if ((tt & QTD_STS_HALTED)) {
                result = (int)t;
                break;
            }
        }
        if (result != -2) break;
    }

    // Unlink and ring the doorbell so the HC drops any cached QH state.
    hc->qh_head.hlink = qh->hlink;
    __sync_synchronize();
    ehci_op_write(hc, EHCI_USBCMD,
                  ehci_op_read(hc, EHCI_USBCMD) | EHCI_CMD_IAAD);
    for (uint32_t i = 0; i < 4000000U; i++) {
        if (ehci_op_read(hc, EHCI_USBSTS) & EHCI_STS_IAA) break;
    }
    ehci_op_write(hc, EHCI_USBSTS, EHCI_STS_IAA);

    return result;
}

static inline uint32_t qtd_remaining(volatile const struct ehci_qtd *q) {
    return (q->token >> QTD_LEN_SHIFT) & 0x7FFF;
}

// ---------------------------------------------------------------------------
// usb_hcd_ops
// ---------------------------------------------------------------------------

static int ehci_control_xfer(struct usb_device *dev,
                             const struct usb_setup_packet *setup, void *data) {
    struct ehci_hc *hc = (struct ehci_hc *)dev->hc;
    bool dir_in = (setup->bmRequestType & 0x80) != 0;
    uint32_t wlen = setup->wLength;
    uint16_t mps = dev->ep0_mps ? dev->ep0_mps : 64;

    if (wlen > sizeof(hc->bounce)) return USB_ERR_IO;
    memcpy(hc->setup_buf, setup, 8);
    if (wlen && !dir_in && data) memcpy(hc->bounce, data, wlen);

    hc->qh_work.epchar = ((uint32_t)dev->addr & 0x7F) |
                         QH_EPS_HIGH | QH_DTC | ((uint32_t)mps << 16);
    hc->qh_work.epcap = QH_MULT_1;

    uint32_t n = 0;
    // SETUP
    ehci_fill_qtd(&hc->qtd[n], 0, QTD_PID_SETUP, 0, hc->setup_buf, 8, false);
    n++;
    // DATA (single qTD, the HC segments into MPS packets; toggle starts at 1)
    if (wlen) {
        ehci_fill_qtd(&hc->qtd[n], 0, dir_in ? QTD_PID_IN : QTD_PID_OUT,
                      1, hc->bounce, wlen, false);
        n++;
    }
    // STATUS: OUT only after an IN data stage; IN otherwise. Toggle 1, IOC.
    ehci_fill_qtd(&hc->qtd[n], EHCI_LINK_TERM,
                  (dir_in && wlen) ? QTD_PID_OUT : QTD_PID_IN,
                  1, NULL, 0, true);
    n++;
    for (uint32_t i = 0; i + 1 < n; i++) {
        hc->qtd[i].next = ehci_dma(&hc->qtd[i + 1]);
    }

    int rc = ehci_run_async(hc, n, 500);
    if (rc == -2) return USB_ERR_TIMEOUT;
    if (rc >= 0) {
        uint32_t tok = hc->qtd[rc].token;
        return (tok & (QTD_STS_BABBLE | QTD_STS_XACTERR))
            ? USB_ERR_IO : USB_ERR_STALL;
    }

    uint32_t got = 0;
    if (wlen) got = wlen - qtd_remaining(&hc->qtd[1]);
    if (dir_in && data && got) memcpy(data, hc->bounce, got);
    return (int)got;
}

static int ehci_bulk_xfer(struct usb_device *dev, uint8_t ep_addr,
                          void *data, uint32_t len) {
    struct ehci_hc *hc = (struct ehci_hc *)dev->hc;
    bool dir_in = (ep_addr & 0x80) != 0;
    uint16_t mps = ehci_ep_mps(dev, ep_addr);

    if (len > sizeof(hc->bounce)) return USB_ERR_IO;
    if (!dir_in && data && len) memcpy(hc->bounce, data, len);

    hc->qh_work.epchar = ((uint32_t)dev->addr & 0x7F) |
                         (((uint32_t)ep_addr & 0x0F) << 8) |
                         QH_EPS_HIGH | QH_DTC | ((uint32_t)mps << 16);
    hc->qh_work.epcap = QH_MULT_1;

    uint8_t toggle = usb_toggle_get(dev, ep_addr);
    ehci_fill_qtd(&hc->qtd[0], EHCI_LINK_TERM,
                  dir_in ? QTD_PID_IN : QTD_PID_OUT,
                  toggle, hc->bounce, len, true);

    int rc = ehci_run_async(hc, 1, 2000);
    if (rc == -2) return USB_ERR_TIMEOUT;
    if (rc >= 0) {
        uint32_t tok = hc->qtd[0].token;
        return (tok & (QTD_STS_BABBLE | QTD_STS_XACTERR))
            ? USB_ERR_IO : USB_ERR_STALL;
    }

    uint32_t got = len - qtd_remaining(&hc->qtd[0]);
    // The HC flipped the toggle once per completed packet.
    uint32_t npkts = len ? (got + mps - 1) / mps : 1;
    if (npkts == 0) npkts = 1;
    usb_toggle_set(dev, ep_addr, toggle ^ (npkts & 1));

    if (dir_in && data && got) memcpy(data, hc->bounce, got);
    return (int)got;
}

static struct ehci_intr_slot *ehci_find_slot(struct ehci_hc *hc,
                                             struct usb_device *dev,
                                             uint8_t ep_addr, bool alloc) {
    for (uint32_t i = 0; i < EHCI_NUM_INTR; i++) {
        if (hc->intr[i].dev == dev && hc->intr[i].ep_addr == ep_addr) {
            return &hc->intr[i];
        }
    }
    if (!alloc) return NULL;
    for (uint32_t i = 0; i < EHCI_NUM_INTR; i++) {
        if (!hc->intr[i].dev) {
            hc->intr[i].dev = dev;
            hc->intr[i].ep_addr = ep_addr;
            return &hc->intr[i];
        }
    }
    return NULL;
}

static int ehci_int_submit(struct usb_device *dev, uint8_t ep_addr,
                           uint32_t len) {
    struct ehci_hc *hc = (struct ehci_hc *)dev->hc;
    struct ehci_intr_slot *slot = ehci_find_slot(hc, dev, ep_addr, true);
    if (!slot || len > sizeof(slot->buf)) return USB_ERR_IO;
    if (slot->active) return 0;

    uint32_t si = (uint32_t)(slot - hc->intr);
    struct ehci_qh *qh = &hc->qh_intr[si];

    slot->len = (uint16_t)len;
    ehci_fill_qtd(&slot->qtd, EHCI_LINK_TERM, QTD_PID_IN,
                  usb_toggle_get(dev, ep_addr), slot->buf, len, true);

    qh->epchar = ((uint32_t)dev->addr & 0x7F) |
                 (((uint32_t)ep_addr & 0x0F) << 8) |
                 QH_EPS_HIGH | QH_DTC |
                 ((uint32_t)ehci_ep_mps(dev, ep_addr) << 16);
    qh->epcap = QH_MULT_1 | 0x01;   // S-mask: poll in microframe 0
    qh->current = 0;
    qh->ov_altnext = EHCI_LINK_TERM;
    qh->ov_token = 0;
    __sync_synchronize();
    qh->ov_next = ehci_dma(&slot->qtd);
    slot->active = true;
    return 0;
}

static int ehci_int_poll(struct usb_device *dev, uint8_t ep_addr,
                         void *buf, uint32_t len) {
    struct ehci_hc *hc = (struct ehci_hc *)dev->hc;
    struct ehci_intr_slot *slot = ehci_find_slot(hc, dev, ep_addr, false);
    if (!slot || !slot->active) return USB_ERR_IO;

    uint32_t tok = ((volatile struct ehci_qtd *)&slot->qtd)->token;
    if (tok & QTD_STS_ACTIVE) return USB_ERR_PENDING;

    slot->active = false;
    if (tok & QTD_STS_HALTED) {
        return (tok & (QTD_STS_BABBLE | QTD_STS_XACTERR))
            ? USB_ERR_IO : USB_ERR_STALL;
    }

    usb_toggle_set(dev, ep_addr, usb_toggle_get(dev, ep_addr) ^ 1);
    uint32_t got = slot->len - ((tok >> QTD_LEN_SHIFT) & 0x7FFF);
    if (got > len) got = len;
    if (got) memcpy(buf, slot->buf, got);
    return (int)got;
}

static const struct usb_hcd_ops g_ehci_ops = {
    .control = ehci_control_xfer,
    .bulk = ehci_bulk_xfer,
    .int_submit = ehci_int_submit,
    .int_poll = ehci_int_poll,
};

// ---------------------------------------------------------------------------
// Controller bring-up
// ---------------------------------------------------------------------------

static void ehci_port_scan(struct ehci_hc *hc, bool ppc) {
    for (uint32_t p = 0; p < hc->nports; p++) {
        uint32_t reg = EHCI_PORTSC + p * 4;
        uint32_t sc = ehci_op_read(hc, reg);

        if (ppc && !(sc & EHCI_PORT_PP)) {
            ehci_op_write(hc, reg, (sc & ~EHCI_PORT_W1C) | EHCI_PORT_PP);
            pit_delay_ms(20);
            sc = ehci_op_read(hc, reg);
        }

        if (!(sc & EHCI_PORT_CCS)) continue;

        // A K-state line means a low-speed device: hand it to the companion.
        if ((sc & EHCI_PORT_LINE_MASK) == EHCI_PORT_LINE_K) {
            kprintf("EHCI: port %u low-speed device -> companion\n",
                    (unsigned int)(p + 1));
            ehci_op_write(hc, reg, (sc & ~EHCI_PORT_W1C) | EHCI_PORT_OWNER);
            continue;
        }

        // Reset. If the port doesn't come up enabled, the device is
        // full-speed: release it to the companion controller.
        sc = ehci_op_read(hc, reg);
        ehci_op_write(hc, reg,
                      ((sc & ~EHCI_PORT_W1C) & ~EHCI_PORT_PED) | EHCI_PORT_PR);
        pit_delay_ms(50);
        sc = ehci_op_read(hc, reg);
        ehci_op_write(hc, reg, (sc & ~EHCI_PORT_W1C) & ~EHCI_PORT_PR);
        for (int i = 0; i < 100; i++) {
            if (!(ehci_op_read(hc, reg) & EHCI_PORT_PR)) break;
            pit_delay_ms(1);
        }
        pit_delay_ms(10);

        sc = ehci_op_read(hc, reg);
        if (!(sc & EHCI_PORT_PED)) {
            kprintf("EHCI: port %u full-speed device -> companion\n",
                    (unsigned int)(p + 1));
            ehci_op_write(hc, reg, (sc & ~EHCI_PORT_W1C) | EHCI_PORT_OWNER);
            continue;
        }

        ehci_op_write(hc, reg, sc | EHCI_PORT_CSC | EHCI_PORT_PEDC);
        kprintf("EHCI: port %u connected (high speed)\n",
                (unsigned int)(p + 1));

        struct usb_device *dev = usb_alloc_device(&g_ehci_ops, hc, NULL,
                                                  USB_SPEED_HIGH);
        if (!dev) return;
        if (usb_enumerate(dev) != 0) {
            usb_free_device(dev);
        }
    }
}

bool ehci_init_controller(struct usb_controller *ctrl) {
    if (!ctrl->bar0.valid || !ctrl->bar0.is_mmio) {
        kprintf("EHCI: no usable MMIO BAR0\n");
        return false;
    }

    struct ehci_hc *hc = NULL;
    for (uint32_t i = 0; i < EHCI_MAX_HC; i++) {
        if (!g_ehci[i].used) { hc = &g_ehci[i]; break; }
    }
    if (!hc) {
        kprintf("EHCI: controller table full\n");
        return false;
    }
    memset(hc, 0, sizeof(*hc));
    hc->used = true;

    uint64_t virt = vmm_map_mmio(ctrl->bar0.address, ctrl->bar0.size);
    if (!virt) {
        kprintf("EHCI: failed to map MMIO\n");
        hc->used = false;
        return false;
    }
    hc->cap = (volatile uint8_t *)virt;
    hc->op = hc->cap + *(volatile uint8_t *)(hc->cap + EHCI_CAPLENGTH);
    ctrl->mmio_virt = virt;
    ctrl->mmio_size = ctrl->bar0.size;

    uint32_t hcsparams = *(volatile uint32_t *)(hc->cap + EHCI_HCSPARAMS);
    hc->nports = hcsparams & 0xF;
    bool ppc = (hcsparams & (1 << 4)) != 0;
    ctrl->max_ports = (uint8_t)hc->nports;

    // Halt + reset.
    ehci_op_write(hc, EHCI_USBCMD,
                  ehci_op_read(hc, EHCI_USBCMD) & ~(uint32_t)EHCI_CMD_RS);
    for (int i = 0; i < 200; i++) {
        if (ehci_op_read(hc, EHCI_USBSTS) & EHCI_STS_HCH) break;
        pit_delay_ms(1);
    }
    ehci_op_write(hc, EHCI_USBCMD, EHCI_CMD_HCRESET);
    for (int i = 0; i < 500; i++) {
        if (!(ehci_op_read(hc, EHCI_USBCMD) & EHCI_CMD_HCRESET)) break;
        pit_delay_ms(1);
    }

    // Async list: head QH pointing at itself, H=1, no work.
    hc->qh_head.hlink = ehci_dma(&hc->qh_head) | EHCI_LINK_QH;
    hc->qh_head.epchar = QH_HEAD;
    hc->qh_head.epcap = QH_MULT_1;
    hc->qh_head.ov_next = EHCI_LINK_TERM;
    hc->qh_head.ov_altnext = EHCI_LINK_TERM;
    hc->qh_head.ov_token = QTD_STS_HALTED;

    // Periodic list: chain of interrupt QHs (idle until armed).
    for (uint32_t i = 0; i < EHCI_NUM_INTR; i++) {
        struct ehci_qh *qh = &hc->qh_intr[i];
        qh->hlink = (i + 1 < EHCI_NUM_INTR)
            ? ehci_dma(&hc->qh_intr[i + 1]) | EHCI_LINK_QH
            : EHCI_LINK_TERM;
        qh->epchar = QH_EPS_HIGH | QH_DTC;   // addr 0 / ep 0: never matches
        qh->epcap = QH_MULT_1 | 0x01;
        qh->ov_next = EHCI_LINK_TERM;
        qh->ov_altnext = EHCI_LINK_TERM;
        qh->ov_token = QTD_STS_HALTED;
    }
    for (uint32_t i = 0; i < 1024; i++) {
        hc->framelist[i] = ehci_dma(&hc->qh_intr[0]) | EHCI_LINK_QH;
    }

    ehci_op_write(hc, EHCI_CTRLDSSEGMENT, 0);
    ehci_op_write(hc, EHCI_USBINTR, 0);
    ehci_op_write(hc, EHCI_PERIODICLISTBASE, ehci_dma(hc->framelist));
    ehci_op_write(hc, EHCI_ASYNCLISTADDR, ehci_dma(&hc->qh_head));
    ehci_op_write(hc, EHCI_USBCMD,
                  EHCI_CMD_RS | EHCI_CMD_ASE | EHCI_CMD_PSE | EHCI_CMD_ITC8);
    ehci_op_write(hc, EHCI_CONFIGFLAG, 1);   // claim all ports
    pit_delay_ms(20);

    kprintf("EHCI: running, %u root ports%s\n",
            (unsigned int)hc->nports, ppc ? " (port power switching)" : "");

    ehci_port_scan(hc, ppc);
    return true;
}
