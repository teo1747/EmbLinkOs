#include "framebuffer.h"
#include "serial.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include <stdint.h>


static fb_info_t fb;
static uint8_t *fb_virt = 0;  // virtual address of framebuffer

void fb_init(void) {
    // get framebuffer info from VBE
    serial_write_string("\n=== Framebuffer init ===\n");

    // Step 1: Read fb_info structure from the physical address 0x6000
    // Use KP2V to convert physical address to kernel virtual address
    fb_info_t *src = (fb_info_t *)KP2V(VBE_INFO_ADDRESS);

    // Step 2: Copy fb_info structure to fb_info variable
    fb = *src;
    serial_write_string("FB phys address: ");
    serial_write_hex(fb.address);
    serial_write_string("\nResolution: ");
    serial_write_hex(fb.width);
    serial_write_string("x");
    serial_write_hex(fb.height);
    serial_write_string("\nPitch: ");
    serial_write_hex(fb.pitch);
    serial_write_string("\nBPP: ");
    serial_write_hex(fb.bpp);
    serial_write_string("\nFormat: ");
    serial_write_hex(fb.format);
    serial_write_string("\n");

    // Step 2: Map framebuffer into virtual memory via MMIO range
    uint64_t fb_size = (uint64_t)fb.pitch * fb.height;
    uint64_t virt = vmm_map_mmio(fb.address, fb_size);

    if (!virt) {
        serial_write_string("Fatal: Failed to map framebuffer into virtual memory\n");
        while (1) {}
    }

    fb_virt = (uint8_t *)virt;  // store virtual address of framebuffer
    serial_write_string("Framebuffer mapped into virtual memory at: ");
    serial_write_hex(virt);
    serial_write_string("\n");

}

void fb_put_pixel(uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b) {
    if (x >= fb.width || y >= fb.height) {
        return;
    
    }

    uint32_t bytes_per_pixel = fb.bpp / 8;
    uint8_t *pixel = fb_virt + (y * fb.pitch) + (x * bytes_per_pixel); // calculate pixel address

    if (fb.format == FB_FORMAT_RGB) {
        pixel[0] = r;
        pixel[1] = g;
        pixel[2] = b;
    } else if (fb.format == FB_FORMAT_BGR) {
        pixel[0] = b;
        pixel[1] = g;
        pixel[2] = r;
    }
}

void fb_clear(uint8_t r, uint8_t g, uint8_t b) {
    for (uint32_t y = 0; y < fb.height; y++) {
        for (uint32_t x = 0; x < fb.width; x++) {
            fb_put_pixel(x, y, r, g, b);
        }
    }
}