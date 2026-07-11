// Bochs/QEMU "stdvga" DISPI display driver.
//
// QEMU's standard VGA (PCI 1234:1111) and Bochs expose the DISPI interface:
// an index/data register pair on I/O ports 0x1CE/0x1CF that programs a linear
// framebuffer mode directly, without real-mode VBE calls. This lets the
// kernel pick its own resolution at runtime; the LFB itself is PCI BAR0.

#include "drivers/video/gpu.h"
#include "drivers/video/framebuffer.h"
#include "drivers/bus/pci.h"
#include "drivers/char/serial.h"
#include "include/io.h"
#include "include/kprintf.h"

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID          0x0
#define VBE_DISPI_INDEX_XRES        0x1
#define VBE_DISPI_INDEX_YRES        0x2
#define VBE_DISPI_INDEX_BPP         0x3
#define VBE_DISPI_INDEX_ENABLE      0x4
#define VBE_DISPI_INDEX_BANK        0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x7
#define VBE_DISPI_INDEX_X_OFFSET    0x8
#define VBE_DISPI_INDEX_Y_OFFSET    0x9

#define VBE_DISPI_ID0        0xB0C0
#define VBE_DISPI_ID5        0xB0C5
#define VBE_DISPI_DISABLED   0x00
#define VBE_DISPI_ENABLED    0x01
#define VBE_DISPI_LFB_ENABLED 0x40
#define VBE_DISPI_NOCLEARMEM  0x80

// Default mode programmed at probe time.
#define BOCHS_DEFAULT_W   1024
#define BOCHS_DEFAULT_H   768
#define BOCHS_DEFAULT_BPP 32

static uint64_t g_bochs_lfb_phys = 0;
static uint64_t g_bochs_lfb_size = 0;
static struct gpu_mode g_bochs_mode;
static bool g_bochs_mode_set = false;

static void dispi_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t dispi_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static bool bochs_modeset(uint32_t w, uint32_t h, uint32_t bpp) {
    if (!g_bochs_lfb_phys) return false;
    if ((uint64_t)w * h * (bpp / 8) > g_bochs_lfb_size) {
        kprintf("bochs: %ux%ux%u does not fit in %u KB of VRAM\n",
                (unsigned int)w, (unsigned int)h, (unsigned int)bpp,
                (unsigned int)(g_bochs_lfb_size / 1024));
        return false;
    }

    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)w);
    dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)h);
    dispi_write(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    dispi_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)w);
    dispi_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)h);
    dispi_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    dispi_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    if (dispi_read(VBE_DISPI_INDEX_XRES) != w ||
        dispi_read(VBE_DISPI_INDEX_YRES) != h) {
        kprintf("bochs: modeset %ux%u rejected by device\n",
                (unsigned int)w, (unsigned int)h);
        return false;
    }

    g_bochs_mode.phys_addr = g_bochs_lfb_phys;
    g_bochs_mode.width  = w;
    g_bochs_mode.height = h;
    g_bochs_mode.pitch  = w * (bpp / 8);
    g_bochs_mode.bpp    = bpp;
    g_bochs_mode.format = FB_FORMAT_BGR;   // DISPI 32bpp is B,G,R,X in memory
    g_bochs_mode_set = true;

    kprintf("bochs: mode %ux%ux%u LFB=%p\n",
            (unsigned int)w, (unsigned int)h, (unsigned int)bpp,
            (void *)(uintptr_t)g_bochs_lfb_phys);
    return true;
}

static bool bochs_get_mode(struct gpu_mode *out) {
    if (!g_bochs_mode_set && !bochs_modeset(BOCHS_DEFAULT_W, BOCHS_DEFAULT_H,
                                            BOCHS_DEFAULT_BPP)) {
        return false;
    }
    *out = g_bochs_mode;
    return true;
}

static bool bochs_set_mode(uint32_t w, uint32_t h, struct gpu_mode *out) {
    if (!bochs_modeset(w, h, BOCHS_DEFAULT_BPP)) return false;
    *out = g_bochs_mode;
    return true;
}

static const struct gpu_driver g_bochs_driver = {
    .name = "bochs-dispi",
    .get_mode = bochs_get_mode,
    .set_mode = bochs_set_mode,
    // Plain LFB device: no flush/accel hooks needed, memory writes are live.
};

const struct gpu_driver *bochs_gpu_probe(void) {
    // Look for QEMU stdvga (1234:1111) or a Bochs-compatible VGA controller.
    const struct pci_device *vga = 0;
    for (uint32_t i = 0; i < pci_devices_count(); i++) {
        const struct pci_device *dev = pci_get_device(i);
        if (!dev) continue;
        if (dev->vendor_id == 0x1234 && dev->device_id == 0x1111) {
            vga = dev;
            break;
        }
    }
    if (!vga) return 0;

    // Sanity-check the DISPI interface version.
    uint16_t id = dispi_read(VBE_DISPI_INDEX_ID);
    if (id < VBE_DISPI_ID0 || id > (VBE_DISPI_ID5 + 0x0F)) {
        kprintf("bochs: stdvga found but DISPI id=%x unsupported\n",
                (unsigned int)id);
        return 0;
    }

    struct pci_bar bar0 = pci_read_bar(vga->bus, vga->device, vga->function, 0);
    if (!bar0.valid || !bar0.is_mmio) {
        kprintf("bochs: stdvga BAR0 unusable\n");
        return 0;
    }

    g_bochs_lfb_phys = bar0.address;
    g_bochs_lfb_size = bar0.size;
    kprintf("bochs: DISPI id=%x VRAM=%u KB at %p\n",
            (unsigned int)id, (unsigned int)(g_bochs_lfb_size / 1024),
            (void *)(uintptr_t)g_bochs_lfb_phys);
    return &g_bochs_driver;
}
