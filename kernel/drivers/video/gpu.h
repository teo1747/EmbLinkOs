#ifndef _GPU_H_
#define _GPU_H_

#include <stdint.h>
#include "include/types.h"

// ---------------------------------------------------------------------------
// GPU abstraction. A display driver (Bochs/QEMU stdvga DISPI, VirtIO-GPU, ...)
// registers itself during gpu_init(); the framebuffer layer then asks it for
// the active video mode and calls the optional hooks below.
//
// Two scan-out models are supported:
//   * Linear framebuffer (VBE / Bochs DISPI): mode.address is the physical
//     VRAM aperture; the fb layer maps it and copies dirty rectangles into it.
//   * Guest-memory scan-out (VirtIO-GPU): mode.address is 0. The fb layer
//     hands its backbuffer to the driver via attach_backing(), and every
//     dirty rectangle is pushed with flush() (TRANSFER_TO_HOST_2D + FLUSH),
//     letting the host GPU do the composition.
// ---------------------------------------------------------------------------

struct gpu_mode {
    uint64_t phys_addr;   // physical LFB address, 0 = guest-memory scan-out
    uint32_t width;
    uint32_t height;
    uint32_t pitch;       // bytes per scanline of the scan-out surface
    uint32_t bpp;
    uint32_t format;      // FB_FORMAT_RGB / FB_FORMAT_BGR
};

struct gpu_driver {
    const char *name;

    // Bring the display up (modeset) and describe the resulting mode.
    bool (*get_mode)(struct gpu_mode *out);

    // Optional: change resolution at runtime. Returns the new mode.
    bool (*set_mode)(uint32_t w, uint32_t h, struct gpu_mode *out);

    // Optional: called by fb_present() after the dirty rect reached the
    // scan-out surface. VirtIO uses this to run the accelerated host blit.
    void (*flush)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    // Optional (guest-memory scan-out only): give the driver the backbuffer
    // so it can attach it as the scan-out resource's backing store.
    bool (*attach_backing)(void *buf, uint64_t size);

    // Optional accelerated 2D ops working directly on the scan-out surface.
    // Return false to make the fb layer fall back to software.
    bool (*fill_rect)(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t rgb);
    bool (*copy_rect)(uint32_t dst_x, uint32_t dst_y,
                      uint32_t src_x, uint32_t src_y,
                      uint32_t w, uint32_t h);
};

// Probe PCI for a supported display device and select the best driver.
// Must run after pci_init()/vmm_init() and before fb_init().
void gpu_init(void);

// Active driver, or NULL when running on the plain VBE framebuffer.
const struct gpu_driver *gpu_active(void);

// Individual driver probes (called by gpu_init in preference order).
const struct gpu_driver *virtio_gpu_probe(void);
const struct gpu_driver *bochs_gpu_probe(void);

#endif /* _GPU_H_ */
