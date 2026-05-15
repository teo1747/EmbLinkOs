#ifndef _FRAMEBUFFER_H_
#define _FRAMEBUFFER_H_

#include <stdint.h>

#define VBE_INFO_ADDRESS 0x6000

#define FB_FORMAT_RGB   0
#define FB_FORMAT_BGR  1


typedef struct fb_info {
    uint64_t fb_address; /* physical/linear address where pixels start */
    uint32_t fb_width;      /* width of the screen */
    uint32_t fb_height;     /* height of the screen */
    uint32_t fb_pitch;      /* bytes per line */
    uint32_t fb_bpp;        /* bits per pixel */    
    uint32_t fb_format;     /* format of the pixels (RGB or RBGR) */
} __attribute__((packed)) fb_info_t;


void fb_init(void);



#endif /* _FRAMEBUFFER_H_ */
