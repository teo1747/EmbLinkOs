#ifndef _FRAMEBUFFER_H_
#define _FRAMEBUFFER_H_

#include <stdint.h>

#define VBE_INFO_ADDRESS 0x6000

#define FB_FORMAT_RGB   0
#define FB_FORMAT_BGR  1


typedef struct fb_info {
    uint64_t address; /* physical/linear address where pixels start */
    uint32_t width;      /* width of the screen */
    uint32_t height;     /* height of the screen */
    uint32_t pitch;      /* bytes per line */
    uint32_t bpp;        /* bits per pixel */    
    uint32_t format;     /* format of the pixels (RGB or RBGR) */
} __attribute__((packed)) fb_info_t;


void fb_init(void);
void fb_put_pixel(uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b);
void fb_clear(uint8_t r, uint8_t g, uint8_t b);

// Draw a single character on the screen at a pixel position(px,py)
void fb_draw_char(char c, uint32_t px, uint32_t py,
                    uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                    uint8_t bg_r, uint8_t bg_g, uint8_t bg_b);


// Draw a null-terminated string on the screen at a pixel position(px,py)
void fb_draw_string(char *str, uint32_t px, uint32_t py,
                    uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                    uint8_t bg_r, uint8_t bg_g, uint8_t bg_b);


// Get a const pointer to the fb_info struct (read-only)
const fb_info_t  *fb_get_info(void);

// Get the virtual address of the mapped framebuffer (read-only)
uint8_t *fb_get_buffer(void);

#endif /* _FRAMEBUFFER_H_ */
