// VirtIO-GPU 2D display driver (QEMU -device virtio-vga / virtio-gpu-pci).
//
// The framebuffer lives in guest RAM (the fb layer's backbuffer) and is
// attached to a host-side GPU resource. Presenting a dirty rectangle issues
// TRANSFER_TO_HOST_2D + RESOURCE_FLUSH on the control virtqueue, and the host
// GPU blits/composites it — that is the acceleration path.
//
// Transport: virtio 1.x "modern" PCI — vendor capabilities point at the
// common/notify/isr/device config windows inside the device's BARs, and the
// control queue is a split virtqueue polled synchronously.

#include "drivers/video/gpu.h"
#include "drivers/video/framebuffer.h"
#include "drivers/bus/pci.h"
#include "drivers/char/serial.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

// ---- virtio PCI capability layout ------------------------------------------
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

// Common config offsets (little-endian MMIO).
#define VC_DEVICE_FEATURE_SELECT 0x00
#define VC_DEVICE_FEATURE        0x04
#define VC_DRIVER_FEATURE_SELECT 0x08
#define VC_DRIVER_FEATURE        0x0C
#define VC_MSIX_CONFIG           0x10
#define VC_NUM_QUEUES            0x12
#define VC_DEVICE_STATUS         0x14
#define VC_CONFIG_GENERATION     0x15
#define VC_QUEUE_SELECT          0x16
#define VC_QUEUE_SIZE            0x18
#define VC_QUEUE_MSIX_VECTOR     0x1A
#define VC_QUEUE_ENABLE          0x1C
#define VC_QUEUE_NOTIFY_OFF      0x1E
#define VC_QUEUE_DESC            0x20
#define VC_QUEUE_DRIVER          0x28
#define VC_QUEUE_DEVICE          0x30

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

#define VIRTIO_F_VERSION_1_BANK 1        // feature bit 32 lives in bank 1 ...
#define VIRTIO_F_VERSION_1_BIT  (1U << 0) // ... as bit 0

// ---- split virtqueue --------------------------------------------------------
#define VQ_SIZE 64

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_SIZE];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VQ_SIZE];
} __attribute__((packed));

// ---- virtio-gpu protocol ----------------------------------------------------
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2   // memory bytes B,G,R,X

#define VIRTIO_GPU_MAX_SCANOUTS 16

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct {
        struct virtio_gpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

// ---- driver state -----------------------------------------------------------
#define VGPU_SCANOUT_RES_ID 1
// Command staging area: attach_backing for an 8 MB framebuffer needs up to
// 2048 mem entries (32 KB) plus the header.
#define VGPU_CMD_BUF_SIZE (40 * 1024)

struct virtio_gpu_state {
    bool present;
    volatile uint8_t *common;         // common config window
    volatile uint8_t *notify_base;    // notify window base
    uint32_t notify_off_multiplier;
    uint16_t queue_notify_off;        // controlq notify offset
    uint16_t vq_size;                 // negotiated queue size
    uint16_t last_used_idx;
    uint16_t free_head;               // next free desc (simple 2-desc usage)

    uint32_t width, height;
    bool scanout_ready;

    struct vring_desc  desc[VQ_SIZE]  __attribute__((aligned(4096)));
    struct vring_avail avail          __attribute__((aligned(4096)));
    struct vring_used  used           __attribute__((aligned(4096)));

    uint8_t cmd_buf[VGPU_CMD_BUF_SIZE] __attribute__((aligned(64)));
    uint8_t resp_buf[4096]             __attribute__((aligned(64)));
};

static struct virtio_gpu_state g_vgpu;

// ---- MMIO helpers -----------------------------------------------------------
static inline uint8_t  vr8(volatile uint8_t *b, uint32_t o)  { return *(volatile uint8_t  *)(b + o); }
static inline uint16_t vr16(volatile uint8_t *b, uint32_t o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t vr32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void vw8(volatile uint8_t *b, uint32_t o, uint8_t v)   { *(volatile uint8_t  *)(b + o) = v; }
static inline void vw16(volatile uint8_t *b, uint32_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void vw32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void vw64(volatile uint8_t *b, uint32_t o, uint64_t v) {
    vw32(b, o, (uint32_t)v);
    vw32(b, o + 4, (uint32_t)(v >> 32));
}

static inline uint64_t vgpu_dma(const volatile void *p) {
    return KV2P((uint64_t)(uintptr_t)p);
}

// ---- synchronous command execution -------------------------------------------
// Places cmd (device-readable) + resp (device-writable) as a two-descriptor
// chain, notifies the device, and busy-waits for the used-ring entry.
static bool vgpu_submit(const void *cmd, uint32_t cmd_len,
                        void *resp, uint32_t resp_len,
                        uint32_t expect_type) {
    struct virtio_gpu_state *s = &g_vgpu;

    if (cmd != s->cmd_buf) memcpy(s->cmd_buf, cmd, cmd_len);
    memset(s->resp_buf, 0, resp_len < sizeof(s->resp_buf) ? resp_len : sizeof(s->resp_buf));

    s->desc[0].addr  = vgpu_dma(s->cmd_buf);
    s->desc[0].len   = cmd_len;
    s->desc[0].flags = VRING_DESC_F_NEXT;
    s->desc[0].next  = 1;
    s->desc[1].addr  = vgpu_dma(s->resp_buf);
    s->desc[1].len   = resp_len;
    s->desc[1].flags = VRING_DESC_F_WRITE;
    s->desc[1].next  = 0;

    uint16_t slot = (uint16_t)(s->avail.idx % s->vq_size);
    s->avail.ring[slot] = 0;
    __sync_synchronize();
    s->avail.idx++;
    __sync_synchronize();

    // Notify the control queue.
    vw16((volatile uint8_t *)s->notify_base,
         (uint32_t)s->queue_notify_off * s->notify_off_multiplier, 0);

    // Busy-wait for completion (control commands are quick on QEMU).
    for (uint32_t spins = 0; spins < 200000000U; spins++) {
        __sync_synchronize();
        if (s->used.idx != s->last_used_idx) {
            s->last_used_idx = s->used.idx;
            struct virtio_gpu_ctrl_hdr *h = (struct virtio_gpu_ctrl_hdr *)s->resp_buf;
            if (h->type != expect_type) {
                kprintf("virtio-gpu: cmd %x -> unexpected resp %x\n",
                        (unsigned int)((const struct virtio_gpu_ctrl_hdr *)s->cmd_buf)->type,
                        (unsigned int)h->type);
                return false;
            }
            if (resp && resp != s->resp_buf) memcpy(resp, s->resp_buf, resp_len);
            return true;
        }
    }
    kprintf("virtio-gpu: command timeout\n");
    return false;
}

static bool vgpu_simple_cmd(const void *cmd, uint32_t cmd_len) {
    struct virtio_gpu_ctrl_hdr resp;
    return vgpu_submit(cmd, cmd_len, &resp, sizeof(resp),
                       VIRTIO_GPU_RESP_OK_NODATA);
}

// ---- transport bring-up -------------------------------------------------------
static volatile uint8_t *vgpu_map_cap(const struct pci_device *dev,
                                      uint8_t bar, uint32_t offset,
                                      uint32_t length) {
    struct pci_bar b = pci_read_bar(dev->bus, dev->device, dev->function, bar);
    if (!b.valid || !b.is_mmio) return 0;
    uint64_t virt = vmm_map_mmio(b.address + offset, length);
    return (volatile uint8_t *)virt;
}

static bool vgpu_init_transport(const struct pci_device *dev) {
    struct virtio_gpu_state *s = &g_vgpu;

    // Walk the PCI capability list for virtio vendor caps (id 0x09).
    uint16_t status = pci_read16(dev->bus, dev->device, dev->function, PCI_STATUS);
    if (!(status & (1 << 4))) {
        kprintf("virtio-gpu: no PCI capability list\n");
        return false;
    }

    uint8_t cap_off = pci_read8(dev->bus, dev->device, dev->function, PCI_CAP_PTR) & 0xFC;
    uint8_t common_bar = 0xFF, notify_bar = 0xFF;
    uint32_t common_off = 0, common_len = 0;
    uint32_t notify_off = 0, notify_len = 0;

    while (cap_off) {
        uint8_t cap_id  = pci_read8(dev->bus, dev->device, dev->function, cap_off);
        uint8_t cap_nxt = pci_read8(dev->bus, dev->device, dev->function, cap_off + 1);
        if (cap_id == 0x09) {   // vendor-specific: virtio structure cap
            uint8_t cfg_type = pci_read8(dev->bus, dev->device, dev->function, cap_off + 3);
            uint8_t bar      = pci_read8(dev->bus, dev->device, dev->function, cap_off + 4);
            uint32_t offset  = pci_read32(dev->bus, dev->device, dev->function, cap_off + 8);
            uint32_t length  = pci_read32(dev->bus, dev->device, dev->function, cap_off + 12);

            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG && common_bar == 0xFF) {
                common_bar = bar; common_off = offset; common_len = length;
            } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG && notify_bar == 0xFF) {
                notify_bar = bar; notify_off = offset; notify_len = length;
                s->notify_off_multiplier =
                    pci_read32(dev->bus, dev->device, dev->function, cap_off + 16);
            }
        }
        cap_off = cap_nxt & 0xFC;
    }

    if (common_bar == 0xFF || notify_bar == 0xFF) {
        kprintf("virtio-gpu: missing common/notify capability (legacy device?)\n");
        return false;
    }

    s->common = vgpu_map_cap(dev, common_bar, common_off, common_len);
    s->notify_base = vgpu_map_cap(dev, notify_bar, notify_off,
                                  notify_len ? notify_len : 4096);
    if (!s->common || !s->notify_base) {
        kprintf("virtio-gpu: failed to map config windows\n");
        return false;
    }

    pci_enable_bus_mastering(dev->bus, dev->device, dev->function);

    // Reset, then acknowledge + driver.
    vw8(s->common, VC_DEVICE_STATUS, 0);
    for (uint32_t i = 0; i < 1000000 && vr8(s->common, VC_DEVICE_STATUS) != 0; i++) { }
    vw8(s->common, VC_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    vw8(s->common, VC_DEVICE_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // Feature negotiation: we only need VIRTIO_F_VERSION_1.
    vw32(s->common, VC_DEVICE_FEATURE_SELECT, VIRTIO_F_VERSION_1_BANK);
    uint32_t feat_hi = vr32(s->common, VC_DEVICE_FEATURE);
    if (!(feat_hi & VIRTIO_F_VERSION_1_BIT)) {
        kprintf("virtio-gpu: device does not offer VERSION_1\n");
        return false;
    }
    vw32(s->common, VC_DRIVER_FEATURE_SELECT, 0);
    vw32(s->common, VC_DRIVER_FEATURE, 0);
    vw32(s->common, VC_DRIVER_FEATURE_SELECT, VIRTIO_F_VERSION_1_BANK);
    vw32(s->common, VC_DRIVER_FEATURE, VIRTIO_F_VERSION_1_BIT);

    uint8_t st = vr8(s->common, VC_DEVICE_STATUS) | VIRTIO_STATUS_FEATURES_OK;
    vw8(s->common, VC_DEVICE_STATUS, st);
    if (!(vr8(s->common, VC_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("virtio-gpu: FEATURES_OK rejected\n");
        return false;
    }

    // Control queue (index 0).
    vw16(s->common, VC_QUEUE_SELECT, 0);
    uint16_t qsize = vr16(s->common, VC_QUEUE_SIZE);
    if (qsize == 0) {
        kprintf("virtio-gpu: controlq missing\n");
        return false;
    }
    if (qsize > VQ_SIZE) {
        vw16(s->common, VC_QUEUE_SIZE, VQ_SIZE);
        qsize = VQ_SIZE;
    }
    s->vq_size = qsize;
    s->last_used_idx = 0;

    memset(s->desc, 0, sizeof(s->desc));
    memset((void *)&s->avail, 0, sizeof(s->avail));
    memset((void *)&s->used, 0, sizeof(s->used));

    vw64(s->common, VC_QUEUE_DESC,   vgpu_dma(s->desc));
    vw64(s->common, VC_QUEUE_DRIVER, vgpu_dma(&s->avail));
    vw64(s->common, VC_QUEUE_DEVICE, vgpu_dma(&s->used));
    s->queue_notify_off = vr16(s->common, VC_QUEUE_NOTIFY_OFF);
    vw16(s->common, VC_QUEUE_ENABLE, 1);

    st = vr8(s->common, VC_DEVICE_STATUS) | VIRTIO_STATUS_DRIVER_OK;
    vw8(s->common, VC_DEVICE_STATUS, st);

    kprintf("virtio-gpu: transport up, controlq size=%u notify_mult=%u\n",
            (unsigned int)qsize, (unsigned int)s->notify_off_multiplier);
    return true;
}

// ---- gpu_driver hooks ----------------------------------------------------------
static bool vgpu_get_mode(struct gpu_mode *out) {
    struct virtio_gpu_state *s = &g_vgpu;

    // Ask the host for the preferred scanout geometry.
    struct virtio_gpu_ctrl_hdr cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    struct virtio_gpu_resp_display_info info;
    s->width = 1024;
    s->height = 768;
    if (vgpu_submit(&cmd, sizeof(cmd), &info, sizeof(info),
                    VIRTIO_GPU_RESP_OK_DISPLAY_INFO)) {
        if (info.pmodes[0].enabled &&
            info.pmodes[0].r.width >= 320 && info.pmodes[0].r.height >= 200) {
            s->width  = info.pmodes[0].r.width;
            s->height = info.pmodes[0].r.height;
        }
    }
    kprintf("virtio-gpu: scanout %ux%u\n",
            (unsigned int)s->width, (unsigned int)s->height);

    // Create the 2D resource the backbuffer will be attached to.
    struct virtio_gpu_resource_create_2d c2d;
    memset(&c2d, 0, sizeof(c2d));
    c2d.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    c2d.resource_id = VGPU_SCANOUT_RES_ID;
    c2d.format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    c2d.width       = s->width;
    c2d.height      = s->height;
    if (!vgpu_simple_cmd(&c2d, sizeof(c2d))) {
        kprintf("virtio-gpu: RESOURCE_CREATE_2D failed\n");
        return false;
    }

    out->phys_addr = 0;   // guest-memory scan-out: fb layer must attach_backing
    out->width  = s->width;
    out->height = s->height;
    out->pitch  = s->width * 4;
    out->bpp    = 32;
    out->format = FB_FORMAT_BGR;
    return true;
}

static bool vgpu_attach_backing(void *buf, uint64_t size) {
    struct virtio_gpu_state *s = &g_vgpu;

    // Build the scatter list: walk the buffer page by page, translating to
    // physical addresses and coalescing contiguous runs.
    struct virtio_gpu_resource_attach_backing *ab =
        (struct virtio_gpu_resource_attach_backing *)s->cmd_buf;
    struct virtio_gpu_mem_entry *entries =
        (struct virtio_gpu_mem_entry *)(s->cmd_buf + sizeof(*ab));
    uint32_t max_entries =
        (VGPU_CMD_BUF_SIZE - sizeof(*ab)) / sizeof(*entries);

    memset(ab, 0, sizeof(*ab));
    ab->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    ab->resource_id = VGPU_SCANOUT_RES_ID;

    uint32_t n = 0;
    uint64_t va = (uint64_t)(uintptr_t)buf;
    uint64_t end = va + size;
    while (va < end) {
        uint64_t phys = vmm_get_phys(va);
        if (!phys) {
            kprintf("virtio-gpu: backbuffer page unmapped at %p\n",
                    (void *)(uintptr_t)va);
            return false;
        }
        uint64_t chunk = PAGE_SIZE;
        if (va + chunk > end) chunk = end - va;

        if (n > 0 &&
            entries[n - 1].addr + entries[n - 1].length == phys) {
            entries[n - 1].length += (uint32_t)chunk;   // coalesce
        } else {
            if (n >= max_entries) {
                kprintf("virtio-gpu: too many backing entries\n");
                return false;
            }
            entries[n].addr = phys;
            entries[n].length = (uint32_t)chunk;
            entries[n].padding = 0;
            n++;
        }
        va += chunk;
    }
    ab->nr_entries = n;

    uint32_t cmd_len = sizeof(*ab) + n * sizeof(*entries);
    struct virtio_gpu_ctrl_hdr resp;
    if (!vgpu_submit(s->cmd_buf, cmd_len, &resp, sizeof(resp),
                     VIRTIO_GPU_RESP_OK_NODATA)) {
        kprintf("virtio-gpu: ATTACH_BACKING failed\n");
        return false;
    }
    kprintf("virtio-gpu: backing attached (%u entr%s)\n",
            (unsigned int)n, n == 1 ? "y" : "ies");

    // Point scanout 0 at the resource.
    struct virtio_gpu_set_scanout sc;
    memset(&sc, 0, sizeof(sc));
    sc.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    sc.r.width     = s->width;
    sc.r.height    = s->height;
    sc.scanout_id  = 0;
    sc.resource_id = VGPU_SCANOUT_RES_ID;
    if (!vgpu_simple_cmd(&sc, sizeof(sc))) {
        kprintf("virtio-gpu: SET_SCANOUT failed\n");
        return false;
    }

    s->scanout_ready = true;
    return true;
}

static void vgpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    struct virtio_gpu_state *s = &g_vgpu;
    if (!s->scanout_ready || w == 0 || h == 0) return;

    // Host reads the dirty rect out of guest memory ...
    struct virtio_gpu_transfer_to_host_2d t;
    memset(&t, 0, sizeof(t));
    t.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    t.r.x = x; t.r.y = y; t.r.width = w; t.r.height = h;
    t.offset      = (uint64_t)y * (s->width * 4) + (uint64_t)x * 4;
    t.resource_id = VGPU_SCANOUT_RES_ID;
    if (!vgpu_simple_cmd(&t, sizeof(t))) return;

    // ... and flushes it to the display.
    struct virtio_gpu_resource_flush f;
    memset(&f, 0, sizeof(f));
    f.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    f.r.x = x; f.r.y = y; f.r.width = w; f.r.height = h;
    f.resource_id = VGPU_SCANOUT_RES_ID;
    vgpu_simple_cmd(&f, sizeof(f));
}

static const struct gpu_driver g_vgpu_driver = {
    .name = "virtio-gpu",
    .get_mode = vgpu_get_mode,
    .flush = vgpu_flush,
    .attach_backing = vgpu_attach_backing,
};

const struct gpu_driver *virtio_gpu_probe(void) {
    const struct pci_device *gpu = 0;
    for (uint32_t i = 0; i < pci_devices_count(); i++) {
        const struct pci_device *dev = pci_get_device(i);
        if (!dev) continue;
        // virtio-gpu modern id: vendor 0x1AF4, device 0x1040 + 16.
        if (dev->vendor_id == 0x1AF4 && dev->device_id == 0x1050) {
            gpu = dev;
            break;
        }
    }
    if (!gpu) return 0;

    kprintf("virtio-gpu: found at %x:%x.%x\n",
            (unsigned int)gpu->bus, (unsigned int)gpu->device,
            (unsigned int)gpu->function);

    memset(&g_vgpu, 0, sizeof(g_vgpu));
    if (!vgpu_init_transport(gpu)) {
        return 0;
    }
    g_vgpu.present = true;
    return &g_vgpu_driver;
}
