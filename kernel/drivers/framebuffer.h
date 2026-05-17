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



#endif /* _FRAMEBUFFER_H_ */
