#ifndef _FRAMEBUFFER_H_
#define _FRAMEBUFFER_H_

#include <stdint.h>
#include "../include/types.h"

#define VBE_INFO_ADDRESS 0x6000

#define FB_FORMAT_RGB   0   /* memory bytes: R, G, B [, X] */
#define FB_FORMAT_BGR   1   /* memory bytes: B, G, R [, X] (usual PC layout) */


typedef struct fb_info {
    uint64_t address; /* physical/linear address where pixels start */
    uint32_t width;      /* width of the screen */
    uint32_t height;     /* height of the screen */
    uint32_t pitch;      /* bytes per line */
    uint32_t bpp;        /* bits per pixel */
    uint32_t format;     /* format of the pixels (RGB or BGR) */
} __attribute__((packed)) fb_info_t;

/* 32-bit color as 0x00RRGGBB, converted to the native layout internally. */
typedef uint32_t fb_color_t;
#define FB_RGB(r, g, b) ((fb_color_t)(((uint32_t)(r) << 16) | \
                                      ((uint32_t)(g) << 8)  | \
                                      ((uint32_t)(b))))
/* 0xAARRGGBB for the alpha-blending blit path. */
#define FB_ARGB(a, r, g, b) ((uint32_t)(((uint32_t)(a) << 24) | FB_RGB(r, g, b)))

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


// ---- Primitives (all clip against the screen; colors are FB_RGB values) ----
void fb_put_pixel_c(uint32_t x, uint32_t y, fb_color_t c);
fb_color_t fb_get_pixel(uint32_t x, uint32_t y);
void fb_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, fb_color_t c);
void fb_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, fb_color_t c);
void fb_draw_hline(int32_t x, int32_t y, int32_t w, fb_color_t c);
void fb_draw_vline(int32_t x, int32_t y, int32_t h, fb_color_t c);
void fb_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, fb_color_t c);
void fb_draw_circle(int32_t cx, int32_t cy, int32_t radius, fb_color_t c);
void fb_fill_circle(int32_t cx, int32_t cy, int32_t radius, fb_color_t c);

// Copy a rectangle of 0x00RRGGBB (or 0xAARRGGBB) pixels onto the screen.
// `stride` is the source stride in PIXELS (use w when the image is tight).
void fb_blit(int32_t x, int32_t y, int32_t w, int32_t h,
             const uint32_t *argb, uint32_t stride);
// Same, but honours the source alpha channel (0 = transparent, 255 = opaque).
void fb_blit_blend(int32_t x, int32_t y, int32_t w, int32_t h,
                   const uint32_t *argb, uint32_t stride);

// Screen-to-screen copy (handles overlap). Coordinates are clipped.
void fb_copy_rect(int32_t dst_x, int32_t dst_y,
                  int32_t src_x, int32_t src_y, int32_t w, int32_t h);

// Scroll the whole screen up by `pixels` rows, filling the exposed band.
void fb_scroll_up(uint32_t pixels, fb_color_t fill);

// ---- Presentation --------------------------------------------------------
// All drawing goes to a RAM backbuffer when one could be allocated; nothing is
// visible until fb_present() pushes the accumulated dirty rectangle to the
// display (memcpy to VRAM and/or a GPU flush command). When no backbuffer
// exists, drawing hits VRAM directly and fb_present() only does the GPU flush.
void fb_present(void);
void fb_present_all(void);      // force a full-screen push
bool fb_double_buffered(void);

// Get a const pointer to the fb_info struct (read-only)
const fb_info_t  *fb_get_info(void);

// Get the virtual address of the DRAW target (backbuffer if double buffered)
uint8_t *fb_get_buffer(void);

#endif /* _FRAMEBUFFER_H_ */
