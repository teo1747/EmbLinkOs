#include "framebuffer.h"
#include "serial.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include <stdint.h>
#include "font_8x16.h"


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

void fb_draw_char(char c, uint32_t px, uint32_t py, uint8_t fg_r, uint8_t fg_g, uint8_t fg_b, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    
    // pointer to the 16 byte glyph data for the character
    const uint8_t *glyph = &font_8x16[(uint8_t)c * FONT_HEIGHT];

    // loop through each row of the glyph
    for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < FONT_WIDTH; col++) {
            // bit 7 is leftmost pixel, bit 0 is rightmost pixel
            // so we need to iterate through the pixels from left to right
            uint8_t pixel_on = (bits >> (7 - col)) & 1;
            if (pixel_on) {
                fb_put_pixel(px + col, py + row, fg_r, fg_g, fg_b);
            }else {
                fb_put_pixel(px + col, py + row, bg_r, bg_g, bg_b);
            }
        }   
        
    }
}

void fb_draw_string(char *str, uint32_t px, uint32_t py, uint8_t fg_r, uint8_t fg_g, uint8_t fg_b, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    
    uint32_t x = px;
    while (*str) {
        fb_draw_char(*str, x, py, fg_r, fg_g, fg_b, bg_r, bg_g, bg_b);
        x += FONT_WIDTH;
        str++;
    }
} 

const fb_info_t *fb_get_info(void) {
    return &fb;
}

uint8_t *fb_get_buffer(void) {
    return fb_virt;
}


