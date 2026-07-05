#include "usb_core.h"
#include "keyboard.h"

#include "../include/kprintf.h"
#include "../include/kstring.h"
#include "../include/errno.h"
#include "../block/block.h"
#include "pit.h"

// ---------------------------------------------------------------------------
// Device table
// ---------------------------------------------------------------------------

static struct usb_device g_usb_devices[USB_MAX_DEVICES];
static uint8_t g_next_usb_addr = 1;

struct usb_device *usb_alloc_device(const struct usb_hcd_ops *ops,
                                    void *hc, void *hc_priv, uint8_t speed) {
    for (uint32_t i = 0; i < USB_MAX_DEVICES; i++) {
        struct usb_device *dev = &g_usb_devices[i];
        if (dev->in_use) continue;
        memset(dev, 0, sizeof(*dev));
        dev->in_use = true;
        dev->ops = ops;
        dev->hc = hc;
        dev->hc_priv = hc_priv;
        dev->speed = speed;
        dev->addr = 0;
        dev->ep0_mps = (speed == USB_SPEED_HIGH) ? 64 : 8;
        return dev;
    }
    kprintf("USB: device table full\n");
    return NULL;
}

void usb_free_device(struct usb_device *dev) {
    if (dev) dev->in_use = false;
}

const struct usb_ep_info *usb_find_ep(struct usb_device *dev,
                                      uint8_t type, bool dir_in) {
    for (uint32_t i = 0; i < dev->num_eps; i++) {
        const struct usb_ep_info *ep = &dev->eps[i];
        if ((ep->attr & 0x03) != type) continue;
        if (((ep->addr & 0x80) != 0) != dir_in) continue;
        return ep;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Control helpers
// ---------------------------------------------------------------------------

int usb_control(struct usb_device *dev, uint8_t bmRequestType,
                uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                void *data, uint16_t wLength) {
    struct usb_setup_packet setup = {
        .bmRequestType = bmRequestType,
        .bRequest = bRequest,
        .wValue = wValue,
        .wIndex = wIndex,
        .wLength = wLength,
    };
    return dev->ops->control(dev, &setup, data);
}

static int usb_get_descriptor(struct usb_device *dev, uint8_t type,
                              uint8_t index, void *buf, uint16_t len) {
    return usb_control(dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                       (uint16_t)((type << 8) | index), 0, buf, len);
}

static int usb_clear_halt(struct usb_device *dev, uint8_t ep_addr) {
    int rc = usb_control(dev, 0x02, USB_REQ_CLEAR_FEATURE,
                         0 /* ENDPOINT_HALT */, ep_addr, NULL, 0);
    usb_toggle_set(dev, ep_addr, 0);
    return rc;
}

// ---------------------------------------------------------------------------
// HID boot keyboard
// ---------------------------------------------------------------------------

// USB HID usage-ID → ASCII (US-QWERTY printable subset, boot protocol).
static const char g_usb_hid_ascii[128] = {
    0,    0,    0,    0,    'a',  'b',  'c',  'd',
    'e',  'f',  'g',  'h',  'i',  'j',  'k',  'l',
    'm',  'n',  'o',  'p',  'q',  'r',  's',  't',
    'u',  'v',  'w',  'x',  'y',  'z',  '1',  '2',
    '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',
    '\n', 0x1B, '\b', '\t', ' ',  '-',  '=',  '[',
    ']',  '\\', 0,    ';',  '\'', '`',  ',',  '.',
    '/',  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    '/',  '*',  '-',  '+',
    '\n', '1',  '2',  '3',  '4',  '5',  '6',  '7',
    '8',  '9',  '0',  '.',
};

static const char g_usb_hid_ascii_shift[128] = {
    0,    0,    0,    0,    'A',  'B',  'C',  'D',
    'E',  'F',  'G',  'H',  'I',  'J',  'K',  'L',
    'M',  'N',  'O',  'P',  'Q',  'R',  'S',  'T',
    'U',  'V',  'W',  'X',  'Y',  'Z',  '!',  '@',
    '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',
    '\n', 0x1B, '\b', '\t', ' ',  '_',  '+',  '{',
    '}',  '|',  0,    ':',  '"',  '~',  '<',  '>',
    '?',  0,    0,    0,    0,    0,    0,    0,
};

static void usb_hid_process_report(struct usb_device *dev,
                                   const uint8_t *report, int len) {
    if (len < 3) return;
    uint8_t modifiers = report[0];
    bool shift = (modifiers & 0x22) != 0;   // L/R shift

    // Edge-detect: inject only keys not present in the previous report.
    for (int i = 2; i < 8 && i < len; i++) {
        uint8_t usage = report[i];
        if (usage == 0 || usage >= 128) continue;
        bool was_down = false;
        for (int j = 0; j < 6; j++) {
            if (dev->prev_keys[j] == usage) { was_down = true; break; }
        }
        if (was_down) continue;
        char ch = shift ? g_usb_hid_ascii_shift[usage] : g_usb_hid_ascii[usage];
        if (!ch) ch = g_usb_hid_ascii[usage];
        if (ch) keyboard_inject_char(ch);
    }

    for (int j = 0; j < 6; j++) {
        dev->prev_keys[j] = (2 + j < len) ? report[2 + j] : 0;
    }
}

static bool usb_hid_attach(struct usb_device *dev) {
    const struct usb_ep_info *ep = usb_find_ep(dev, 3, true);
    if (!ep) {
        kprintf("USB HID: no interrupt-IN endpoint\n");
        return false;
    }

    // Boot protocol + infinite idle so the device only reports changes.
    usb_control(dev, 0x21, 0x0B /* SET_PROTOCOL */, 0 /* boot */, 0, NULL, 0);
    usb_control(dev, 0x21, 0x0A /* SET_IDLE */, 0, 0, NULL, 0);

    dev->hid_ep = ep->addr;
    dev->hid_mps = ep->mps > 8 ? 8 : ep->mps;   // boot report is 8 bytes
    if (dev->hid_mps == 0) dev->hid_mps = 8;

    if (dev->ops->int_submit(dev, dev->hid_ep, dev->hid_mps) < 0) {
        kprintf("USB HID: failed to arm interrupt endpoint\n");
        return false;
    }
    dev->hid_active = true;
    kprintf("USB HID: boot keyboard ready (addr %u, ep 0x%x)\n",
            (unsigned int)dev->addr, (unsigned int)dev->hid_ep);
    return true;
}

void usb_core_poll(void) {
    for (uint32_t i = 0; i < USB_MAX_DEVICES; i++) {
        struct usb_device *dev = &g_usb_devices[i];
        if (!dev->in_use || !dev->hid_active) continue;

        uint8_t report[8];
        int rc = dev->ops->int_poll(dev, dev->hid_ep, report, sizeof(report));
        if (rc == USB_ERR_PENDING) continue;
        if (rc >= 0) {
            usb_hid_process_report(dev, report, rc);
        } else if (rc == USB_ERR_STALL) {
            usb_clear_halt(dev, dev->hid_ep);
        }
        // Always re-arm so the stream keeps flowing.
        dev->ops->int_submit(dev, dev->hid_ep, dev->hid_mps);
    }
}

// ---------------------------------------------------------------------------
// Mass storage (Bulk-Only Transport + SCSI)
// ---------------------------------------------------------------------------

#define USB_MSC_MAX_DEVS   4
#define MSC_CHUNK_BYTES    4096U
#define MSC_SECTOR_SIZE    512U

#define MSC_CBW_SIG 0x43425355U
#define MSC_CSW_SIG 0x53425355U

struct usb_msc {
    bool used;
    struct usb_device *dev;
    uint8_t ep_in, ep_out;
    uint32_t tag;
    uint64_t blocks;
    uint32_t block_size;
    struct embk_block_device blk;
    uint8_t data[MSC_CHUNK_BYTES] __attribute__((aligned(64)));
};

static struct usb_msc g_usb_msc[USB_MSC_MAX_DEVS];

static inline void put_be32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static inline uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static inline void put_le32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

// One BOT round-trip: CBW → optional data stage → CSW. Data uses m->data.
static bool usb_msc_bot(struct usb_msc *m, const uint8_t *cdb, uint8_t cdb_len,
                        bool dir_in, uint32_t data_len) {
    struct usb_device *dev = m->dev;
    uint8_t cbw[31];
    memset(cbw, 0, sizeof(cbw));
    put_le32(cbw + 0, MSC_CBW_SIG);
    put_le32(cbw + 4, ++m->tag);
    put_le32(cbw + 8, data_len);
    cbw[12] = dir_in ? 0x80 : 0x00;
    cbw[13] = 0;               // LUN 0
    cbw[14] = cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);

    if (dev->ops->bulk(dev, m->ep_out, cbw, sizeof(cbw)) < 0) {
        return false;
    }

    if (data_len > 0) {
        int rc = dev->ops->bulk(dev, dir_in ? m->ep_in : m->ep_out,
                                m->data, data_len);
        if (rc == USB_ERR_STALL) {
            usb_clear_halt(dev, dir_in ? m->ep_in : m->ep_out);
        } else if (rc < 0) {
            return false;
        }
    }

    uint8_t csw[13];
    int rc = dev->ops->bulk(dev, m->ep_in, csw, sizeof(csw));
    if (rc == USB_ERR_STALL) {
        usb_clear_halt(dev, m->ep_in);
        rc = dev->ops->bulk(dev, m->ep_in, csw, sizeof(csw));
    }
    if (rc < 13) return false;

    uint32_t sig = (uint32_t)csw[0] | ((uint32_t)csw[1]<<8) |
                   ((uint32_t)csw[2]<<16) | ((uint32_t)csw[3]<<24);
    return sig == MSC_CSW_SIG && csw[12] == 0;   // bCSWStatus 0 = passed
}

static bool scsi_rw10(struct usb_msc *m, bool write,
                      uint32_t lba, uint16_t blocks) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = write ? 0x2A : 0x28;
    put_be32(cdb + 2, lba);
    cdb[7] = (uint8_t)(blocks >> 8);
    cdb[8] = (uint8_t)blocks;
    return usb_msc_bot(m, cdb, 10, !write,
                       (uint32_t)blocks * m->block_size);
}

static int usb_msc_block_read(struct embk_block_device *bdev, uint64_t lba,
                              uint32_t count, void *buffer) {
    struct usb_msc *m = (struct usb_msc *)bdev->driver_data;
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t per = MSC_CHUNK_BYTES / m->block_size;
    while (count) {
        uint32_t n = count > per ? per : count;
        size_t bytes = (size_t)n * (size_t)m->block_size;
        if (!scsi_rw10(m, false, (uint32_t)lba, (uint16_t)n)) return -EMBK_EIO;
        memcpy(dst, m->data, bytes);
        dst += bytes;
        lba += n;
        count -= n;
    }
    return EMBK_OK;
}

static int usb_msc_block_write(struct embk_block_device *bdev, uint64_t lba,
                               uint32_t count, const void *buffer) {
    struct usb_msc *m = (struct usb_msc *)bdev->driver_data;
    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t per = MSC_CHUNK_BYTES / m->block_size;
    while (count) {
        uint32_t n = count > per ? per : count;
        memcpy(m->data, src, (size_t)n * (size_t)m->block_size);
        if (!scsi_rw10(m, true, (uint32_t)lba, (uint16_t)n)) return -EMBK_EIO;
        src += n * m->block_size;
        lba += n;
        count -= n;
    }
    return EMBK_OK;
}

static bool usb_msc_attach(struct usb_device *dev) {
    const struct usb_ep_info *in = usb_find_ep(dev, 2, true);
    const struct usb_ep_info *out = usb_find_ep(dev, 2, false);
    if (!in || !out) {
        kprintf("USB MSC: bulk endpoints missing\n");
        return false;
    }

    struct usb_msc *m = NULL;
    for (uint32_t i = 0; i < USB_MSC_MAX_DEVS; i++) {
        if (!g_usb_msc[i].used) { m = &g_usb_msc[i]; break; }
    }
    if (!m) {
        kprintf("USB MSC: device table full\n");
        return false;
    }
    memset(m, 0, sizeof(*m));
    m->used = true;
    m->dev = dev;
    m->ep_in = in->addr;
    m->ep_out = out->addr;

    // INQUIRY (36 bytes) — mostly to kick the device awake.
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12; cdb[4] = 36;
    usb_msc_bot(m, cdb, 6, true, 36);

    // TEST UNIT READY until it settles, clearing sense data in between.
    for (int tries = 0; tries < 5; tries++) {
        memset(cdb, 0, sizeof(cdb));
        if (usb_msc_bot(m, cdb, 6, false, 0)) break;
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x03; cdb[4] = 18;             // REQUEST SENSE
        usb_msc_bot(m, cdb, 6, true, 18);
        pit_delay_ms(10);
    }

    // READ CAPACITY(10)
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    if (!usb_msc_bot(m, cdb, 10, true, 8)) {
        kprintf("USB MSC: READ CAPACITY failed\n");
        m->used = false;
        return false;
    }
    uint32_t max_lba = get_be32(m->data);
    m->block_size = get_be32(m->data + 4);
    if (m->block_size == 0 || m->block_size > MSC_CHUNK_BYTES) {
        kprintf("USB MSC: unsupported block size %u\n",
                (unsigned int)m->block_size);
        m->used = false;
        return false;
    }
    m->blocks = (uint64_t)max_lba + 1;

    m->blk.block_count = m->blocks;
    m->blk.block_size = m->block_size;
    m->blk.read = usb_msc_block_read;
    m->blk.write = usb_msc_block_write;
    m->blk.flush = NULL;
    m->blk.driver_data = m;
    m->blk.dma_max_phys = 0xFFFFFFFFULL;
    m->blk.needs_kernel_range = true;

    if (embk_block_register(&m->blk) != EMBK_OK) {
        kprintf("USB MSC: block registration failed\n");
        m->used = false;
        return false;
    }
    kprintf("USB MSC: %s registered, %u blocks x %u B (%u MB)\n",
            m->blk.name,
            (unsigned int)m->blocks, (unsigned int)m->block_size,
            (unsigned int)((m->blocks * m->block_size) >> 20));
    return true;
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

// Parse the configuration blob: first interface's class triple plus its
// endpoint list (stops at the next interface descriptor).
static void usb_parse_config(struct usb_device *dev,
                             const uint8_t *cfg, uint32_t len) {
    dev->num_eps = 0;
    bool in_first_iface = false;
    uint32_t off = 0;
    while (off + 2 <= len) {
        uint8_t dlen = cfg[off];
        uint8_t dtype = cfg[off + 1];
        if (dlen == 0 || off + dlen > len) break;

        if (dtype == USB_DESC_INTERFACE && dlen >= 9) {
            if (in_first_iface) break;      // second interface: stop
            in_first_iface = true;
            dev->if_class = cfg[off + 5];
            dev->if_subclass = cfg[off + 6];
            dev->if_protocol = cfg[off + 7];
        } else if (dtype == USB_DESC_ENDPOINT && dlen >= 7 && in_first_iface) {
            if (dev->num_eps < USB_MAX_ENDPOINTS) {
                struct usb_ep_info *ep = &dev->eps[dev->num_eps++];
                ep->addr = cfg[off + 2];
                ep->attr = cfg[off + 3];
                ep->mps = (uint16_t)(cfg[off + 4] | (cfg[off + 5] << 8)) & 0x7FF;
                ep->interval = cfg[off + 6];
            }
        }
        off += dlen;
    }
}

// ---------------------------------------------------------------------------
// USB hubs (single level; shared by UHCI/OHCI/EHCI via usb_core's HCD-
// agnostic control-transfer path -- a downstream device is just another
// address on the same physical bus/HC as far as usb_hcd_ops is concerned,
// so no per-HC hub routing is needed for that part).
//
// Deliberately NOT handled here: xHCI (which enumerates hubs through its
// own separate legacy code path, not usb_core.c -- still just logs
// "USB Hub detected" and stops, matching its pre-existing "only the first
// device enumerates" limitation) and full/low-speed devices behind a
// high-speed EHCI hub (needs a Transaction Translator -- EHCI spec §4.14 --
// a genuinely separate, larger feature; only same-speed downstream devices
// are enumerated here). Both are documented gaps, not silent ones.
// ---------------------------------------------------------------------------

#define USB_DESC_HUB 0x29

// Class-specific hub requests (USB 2.0 spec §11.24.2). bmRequestType:
// 0xA0 = device-to-host/class/device (descriptor fetch),
// 0xA3 = device-to-host/class/other (port status),
// 0x23 = host-to-device/class/other (port feature set/clear).
#define USB_HUB_REQ_GET_STATUS         0x00
#define USB_HUB_REQ_CLEAR_FEATURE      0x01
#define USB_HUB_REQ_SET_FEATURE        0x03

#define USB_PORT_FEAT_CONNECTION       0
#define USB_PORT_FEAT_ENABLE           1
#define USB_PORT_FEAT_RESET            4
#define USB_PORT_FEAT_POWER            8
#define USB_PORT_FEAT_LOW_SPEED        9
#define USB_PORT_FEAT_C_CONNECTION     16
#define USB_PORT_FEAT_C_RESET          20

#define USB_MAX_HUB_DEPTH 4   // defensive bound against a malformed/looping topology

static uint8_t g_hub_depth = 0;

// Class-specific hub descriptor (USB 2.0 spec §11.23.2.1). Only the fixed
// leading fields are needed here (port count); the variable-length
// DeviceRemovable/PortPwrCtrlMask bitmaps that follow are unused.
struct usb_hub_descriptor {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;   // USB_DESC_HUB
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;
    uint8_t  bHubContrCurrent;
} __attribute__((packed));

static bool usb_hub_get_port_status(struct usb_device *hub, uint8_t port,
                                    uint16_t *status, uint16_t *change) {
    uint8_t buf[4];
    int rc = usb_control(hub, 0xA3, USB_HUB_REQ_GET_STATUS, 0, port, buf, 4);
    if (rc < 4) return false;
    *status = (uint16_t)(buf[0] | (buf[1] << 8));
    *change = (uint16_t)(buf[2] | (buf[3] << 8));
    return true;
}

static void usb_hub_set_port_feature(struct usb_device *hub, uint8_t port, uint16_t feature) {
    usb_control(hub, 0x23, USB_HUB_REQ_SET_FEATURE, feature, port, NULL, 0);
}

static void usb_hub_clear_port_feature(struct usb_device *hub, uint8_t port, uint16_t feature) {
    usb_control(hub, 0x23, USB_HUB_REQ_CLEAR_FEATURE, feature, port, NULL, 0);
}

// Bring up one downstream port: power it, reset it, read back the resulting
// speed, and enumerate whatever's attached. Mirrors each HCD's own root-port
// scan (uhci_port_scan() et al.) but through class-specific hub requests
// instead of the HC's native port registers.
static void usb_hub_port_attach(struct usb_device *hub, uint8_t port) {
    usb_hub_set_port_feature(hub, port, USB_PORT_FEAT_POWER);
    pit_delay_ms(20);   // bPwrOn2PwrGood is in 2 ms units; 20 ms covers any real hub

    uint16_t status, change;
    if (!usb_hub_get_port_status(hub, port, &status, &change)) return;
    if (!(status & (1 << USB_PORT_FEAT_CONNECTION))) return;   // nothing plugged in

    usb_hub_set_port_feature(hub, port, USB_PORT_FEAT_RESET);
    pit_delay_ms(50);

    if (!usb_hub_get_port_status(hub, port, &status, &change)) return;

    // W1C the change bits the reset/connect sequence set, same as a root port.
    if (change & (1 << (USB_PORT_FEAT_C_RESET - 16)))
        usb_hub_clear_port_feature(hub, port, USB_PORT_FEAT_C_RESET);
    if (change & (1 << (USB_PORT_FEAT_C_CONNECTION - 16)))
        usb_hub_clear_port_feature(hub, port, USB_PORT_FEAT_C_CONNECTION);

    if (!(status & (1 << USB_PORT_FEAT_ENABLE))) {
        kprintf("USB hub: port %u failed to enable\n", (unsigned int)port);
        return;
    }

    uint8_t speed = (status & (1 << USB_PORT_FEAT_LOW_SPEED)) ? USB_SPEED_LOW : USB_SPEED_FULL;
    kprintf("USB hub: port %u connected (%s speed)\n", (unsigned int)port,
            speed == USB_SPEED_LOW ? "low" : "full");

    struct usb_device *child = usb_alloc_device(hub->ops, hub->hc, NULL, speed);
    if (!child) return;
    if (usb_enumerate(child) != 0) {
        usb_free_device(child);
    }
}

static bool usb_hub_attach(struct usb_device *hub) {
    if (g_hub_depth >= USB_MAX_HUB_DEPTH) {
        kprintf("USB hub: max nesting depth reached, not descending further\n");
        return false;
    }

    struct usb_hub_descriptor hd;
    int rc = usb_control(hub, 0xA0, USB_REQ_GET_DESCRIPTOR,
                         (uint16_t)(USB_DESC_HUB << 8), 0, &hd, sizeof(hd));
    if (rc < (int)sizeof(hd)) {
        kprintf("USB hub: GET_DESCRIPTOR failed (%d)\n", rc);
        return false;
    }

    kprintf("USB hub: %u downstream port(s)\n", (unsigned int)hd.bNbrPorts);

    g_hub_depth++;
    for (uint8_t port = 1; port <= hd.bNbrPorts; port++) {
        usb_hub_port_attach(hub, port);
    }
    g_hub_depth--;
    return true;
}

int usb_enumerate(struct usb_device *dev) {
    static uint8_t cfg_buf[256];
    uint8_t buf[18];

    // 1) First 8 bytes of the device descriptor to learn EP0's real MPS.
    int rc = usb_get_descriptor(dev, USB_DESC_DEVICE, 0, buf, 8);
    if (rc < 8) {
        kprintf("USB: GET_DESCRIPTOR(dev,8) failed (%d)\n", rc);
        return -1;
    }
    if (buf[7] >= 8) dev->ep0_mps = buf[7];

    // 2) Assign an address.
    uint8_t addr = g_next_usb_addr++;
    if (g_next_usb_addr > 127) g_next_usb_addr = 1;
    rc = usb_control(dev, 0x00, USB_REQ_SET_ADDRESS, addr, 0, NULL, 0);
    if (rc < 0) {
        kprintf("USB: SET_ADDRESS(%u) failed\n", (unsigned int)addr);
        return -1;
    }
    dev->addr = addr;
    pit_delay_ms(5);   // address settle time (spec: 2 ms)

    // 3) Full device descriptor.
    rc = usb_get_descriptor(dev, USB_DESC_DEVICE, 0, buf, 18);
    if (rc < 18) {
        kprintf("USB: GET_DESCRIPTOR(dev,18) failed (%d)\n", rc);
        return -1;
    }
    dev->vid = (uint16_t)(buf[8] | (buf[9] << 8));
    dev->pid = (uint16_t)(buf[10] | (buf[11] << 8));
    dev->dev_class = buf[4];

    // 4) Configuration descriptor (header, then the whole blob).
    rc = usb_get_descriptor(dev, USB_DESC_CONFIG, 0, cfg_buf, 9);
    if (rc < 9) {
        kprintf("USB: GET_DESCRIPTOR(cfg,9) failed (%d)\n", rc);
        return -1;
    }
    uint16_t total = (uint16_t)(cfg_buf[2] | (cfg_buf[3] << 8));
    if (total > sizeof(cfg_buf)) total = sizeof(cfg_buf);
    rc = usb_get_descriptor(dev, USB_DESC_CONFIG, 0, cfg_buf, total);
    if (rc < 9) {
        kprintf("USB: GET_DESCRIPTOR(cfg,full) failed (%d)\n", rc);
        return -1;
    }
    uint8_t config_value = cfg_buf[5];
    usb_parse_config(dev, cfg_buf, total);

    // 5) Configure.
    rc = usb_control(dev, 0x00, USB_REQ_SET_CONFIGURATION,
                     config_value, 0, NULL, 0);
    if (rc < 0) {
        kprintf("USB: SET_CONFIGURATION failed\n");
        return -1;
    }

    kprintf("USB: addr %u %x:%x class %x/%x/%x eps=%u speed=%s\n",
            (unsigned int)dev->addr,
            (unsigned int)dev->vid, (unsigned int)dev->pid,
            (unsigned int)dev->if_class, (unsigned int)dev->if_subclass,
            (unsigned int)dev->if_protocol, (unsigned int)dev->num_eps,
            dev->speed == USB_SPEED_LOW ? "low" :
            dev->speed == USB_SPEED_FULL ? "full" : "high");

    // 6) Class dispatch.
    if (dev->if_class == 0x03 && dev->if_subclass == 0x01 &&
        dev->if_protocol == 0x01) {
        usb_hid_attach(dev);
    } else if (dev->if_class == 0x03 && dev->if_subclass == 0x01 &&
               dev->if_protocol == 0x02) {
        kprintf("USB: HID boot mouse detected (no driver yet)\n");
    } else if (dev->if_class == 0x08) {
        usb_msc_attach(dev);
    } else if (dev->if_class == 0x09) {
        usb_hub_attach(dev);
    }

    return 0;
}
