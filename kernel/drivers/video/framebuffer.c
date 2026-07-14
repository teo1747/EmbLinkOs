#include "drivers/video/framebuffer.h"
#include "drivers/video/gpu.h"
#include "drivers/char/serial.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "include/kstring.h"
#include "include/kmalloc.h"
#include "include/kprintf.h"
#include <stdint.h>
#include "drivers/video/font_8x16.h"
#include "arch/x86_64/cpu/spinlock.h"


static fb_info_t fb;
static uint8_t *fb_front = 0;   // mapped scan-out surface (VRAM), may be NULL
static uint8_t *fb_back  = 0;   // RAM backbuffer (draw target), may be NULL
static uint8_t *fb_draw  = 0;   // where primitives write (back if available)
static uint32_t fb_bypp  = 4;   // bytes per pixel

// Accumulated dirty rectangle since the last fb_present(). x1/y1 exclusive.
// GUARDED by fb_dirty_lock: under SMP, one core's fb_present() raced another
// core's fb_mark_dirty() between "read the box" and "dirty_any = false" -- the
// second core's region was silently DROPPED and never flushed to the device.
// Observed as a freshly-created window staying black under -smp 4: its first
// full-frame flush lost the race with a concurrent console print, and an idle
// app never repaints, so the region stayed un-transferred forever.
static spinlock_t fb_dirty_lock = SPINLOCK_INIT;
static uint32_t dirty_x0, dirty_y0, dirty_x1, dirty_y1;
static bool     dirty_any = false;

static void fb_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t x1 = x + w, y1 = y + h;
    if (x1 > fb.width)  x1 = fb.width;
    if (y1 > fb.height) y1 = fb.height;
    if (x >= x1 || y >= y1) return;
    spin_lock(&fb_dirty_lock);
    if (!dirty_any) {
        dirty_x0 = x; dirty_y0 = y; dirty_x1 = x1; dirty_y1 = y1;
        dirty_any = true;
        spin_unlock(&fb_dirty_lock);
        return;
    }
    if (x  < dirty_x0) dirty_x0 = x;
    if (y  < dirty_y0) dirty_y0 = y;
    if (x1 > dirty_x1) dirty_x1 = x1;
    if (y1 > dirty_y1) dirty_y1 = y1;
    spin_unlock(&fb_dirty_lock);
}

// Convert 0x00RRGGBB into the native pixel value for the current mode.
static inline uint32_t fb_pack(fb_color_t c) {
    uint8_t r = (uint8_t)(c >> 16), g = (uint8_t)(c >> 8), b = (uint8_t)c;
    switch (fb.bpp) {
    case 32:
    case 24:
        // Little-endian 32-bit store lays bytes out low-to-high.
        if (fb.format == FB_FORMAT_BGR)
            return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;   // B,G,R,X
        return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;       // R,G,B,X
    case 16:
        return (uint32_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    default:
        return c;
    }
}

static inline fb_color_t fb_unpack(uint32_t v) {
    uint8_t r, g, b;
    switch (fb.bpp) {
    case 32:
    case 24:
        if (fb.format == FB_FORMAT_BGR) {
            r = (uint8_t)(v >> 16); g = (uint8_t)(v >> 8); b = (uint8_t)v;
        } else {
            b = (uint8_t)(v >> 16); g = (uint8_t)(v >> 8); r = (uint8_t)v;
        }
        break;
    case 16:
        r = (uint8_t)(((v >> 11) & 0x1F) << 3);
        g = (uint8_t)(((v >> 5)  & 0x3F) << 2);
        b = (uint8_t)((v & 0x1F) << 3);
        break;
    default:
        return v;
    }
    return FB_RGB(r, g, b);
}

static inline uint8_t *fb_pixel_ptr(uint8_t *base, uint32_t x, uint32_t y) {
    return base + (uint64_t)y * fb.pitch + (uint64_t)x * fb_bypp;
}

static inline void fb_store(uint8_t *p, uint32_t native) {
    switch (fb.bpp) {
    case 32: *(uint32_t *)p = native; break;
    case 24: p[0] = (uint8_t)native; p[1] = (uint8_t)(native >> 8);
             p[2] = (uint8_t)(native >> 16); break;
    case 16: *(uint16_t *)p = (uint16_t)native; break;
    default: *p = (uint8_t)native; break;
    }
}

static inline uint32_t fb_load(const uint8_t *p) {
    switch (fb.bpp) {
    case 32: return *(const uint32_t *)p;
    case 24: return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    case 16: return *(const uint16_t *)p;
    default: return *p;
    }
}

// Clip a rect against the screen. Returns false when nothing remains.
static bool fb_clip(int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
    if (*w <= 0 || *h <= 0) return false;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x >= (int32_t)fb.width || *y >= (int32_t)fb.height) return false;
    if (*x + *w > (int32_t)fb.width)  *w = (int32_t)fb.width  - *x;
    if (*y + *h > (int32_t)fb.height) *h = (int32_t)fb.height - *y;
    return (*w > 0 && *h > 0);
}

void fb_init(void) {
    serial_write_string("\n=== Framebuffer init ===\n");

    struct gpu_mode mode;
    const struct gpu_driver *gpu = gpu_active();
    bool have_mode = false;

    if (gpu && gpu->get_mode && gpu->get_mode(&mode)) {
        fb.address = mode.phys_addr;
        fb.width   = mode.width;
        fb.height  = mode.height;
        fb.pitch   = mode.pitch;
        fb.bpp     = mode.bpp;
        fb.format  = mode.format;
        have_mode = true;
        serial_write_string("FB mode from GPU driver\n");
    }

    if (!have_mode) {
        // Fall back to the VBE mode the stage2 loader programmed (info block
        // copied to physical 0x6000).
        fb_info_t *src = (fb_info_t *)KP2V(VBE_INFO_ADDRESS);
        fb = *src;
        serial_write_string("FB mode from VBE\n");
    }

    fb_bypp = fb.bpp / 8;
    if (fb_bypp == 0) fb_bypp = 1;

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

    uint64_t fb_size = (uint64_t)fb.pitch * fb.height;

    // Map the linear framebuffer when the scan-out lives in device memory.
    fb_front = 0;
    if (fb.address) {
        uint64_t virt = vmm_map_mmio(fb.address, fb_size);
        if (!virt) {
            serial_write_string("Fatal: Failed to map framebuffer into virtual memory\n");
            while (1) {}
        }
        fb_front = (uint8_t *)virt;
        serial_write_string("Framebuffer mapped into virtual memory at: ");
        serial_write_hex(virt);
        serial_write_string("\n");
    }

    // Allocate the RAM backbuffer (page aligned so a guest-memory scan-out
    // driver can hand its physical pages to the device).
    fb_back = 0;
    uint8_t *raw = (uint8_t *)kmalloc(fb_size + PAGE_SIZE);
    if (raw) {
        uint64_t aligned = ((uint64_t)(uintptr_t)raw + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        fb_back = (uint8_t *)(uintptr_t)aligned;
        memset(fb_back, 0, fb_size);
        serial_write_string("FB backbuffer allocated (double buffering on)\n");
    } else {
        serial_write_string("FB backbuffer allocation FAILED (drawing direct)\n");
    }

    // Guest-memory scan-out (VirtIO): the backbuffer *is* the display surface.
    if (!fb_front) {
        if (!fb_back || !gpu || !gpu->attach_backing ||
            !gpu->attach_backing(fb_back, fb_size)) {
            serial_write_string("Fatal: no scan-out surface available\n");
            while (1) {}
        }
        serial_write_string("FB backbuffer attached as GPU scan-out resource\n");
    }

    fb_draw = fb_back ? fb_back : fb_front;
    dirty_any = false;

    // Start from a known state on screen.
    fb_clear(0, 0, 0);
    fb_present_all();
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

static void fb_push_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (fb_back && fb_front) {
        uint64_t off = (uint64_t)y * fb.pitch + (uint64_t)x * fb_bypp;
        uint32_t bytes = w * fb_bypp;
        const uint8_t *src = fb_back + off;
        uint8_t *dst = fb_front + off;
        for (uint32_t row = 0; row < h; row++) {
            memcpy(dst, src, bytes);
            src += fb.pitch;
            dst += fb.pitch;
        }
    }
    const struct gpu_driver *gpu = gpu_active();
    if (gpu && gpu->flush) {
        gpu->flush(x, y, w, h);
    }
}

void fb_present(void) {
    /* snapshot + clear ATOMICALLY; push OUTSIDE the lock (the push reaches the
     * virtio submit, whose error path kprintf()s back through the console into
     * fb_present -- holding fb_dirty_lock across it would self-deadlock). A
     * mark that lands after the snapshot re-arms dirty_any and is flushed by
     * the NEXT present -- nothing is lost. */
    spin_lock(&fb_dirty_lock);
    if (!dirty_any) { spin_unlock(&fb_dirty_lock); return; }
    uint32_t x0 = dirty_x0, y0 = dirty_y0, x1 = dirty_x1, y1 = dirty_y1;
    dirty_any = false;
    spin_unlock(&fb_dirty_lock);
    fb_push_rect(x0, y0, x1 - x0, y1 - y0);
}

void fb_present_all(void) {
    spin_lock(&fb_dirty_lock);
    dirty_any = false;
    spin_unlock(&fb_dirty_lock);
    fb_push_rect(0, 0, fb.width, fb.height);
}

bool fb_double_buffered(void) {
    return fb_back != 0;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

void fb_put_pixel_c(uint32_t x, uint32_t y, fb_color_t c) {
    if (x >= fb.width || y >= fb.height) return;
    fb_store(fb_pixel_ptr(fb_draw, x, y), fb_pack(c));
    fb_mark_dirty(x, y, 1, 1);
}

void fb_put_pixel(uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b) {
    fb_put_pixel_c(x, y, FB_RGB(r, g, b));
}

fb_color_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (x >= fb.width || y >= fb.height) return 0;
    return fb_unpack(fb_load(fb_pixel_ptr(fb_draw, x, y)));
}

void fb_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, fb_color_t c) {
    if (!fb_clip(&x, &y, &w, &h)) return;

    uint32_t native = fb_pack(c);
    uint8_t *row = fb_pixel_ptr(fb_draw, (uint32_t)x, (uint32_t)y);

    if (fb.bpp == 32) {
        for (int32_t j = 0; j < h; j++) {
            uint32_t *p = (uint32_t *)row;
            for (int32_t i = 0; i < w; i++) p[i] = native;
            row += fb.pitch;
        }
    } else {
        for (int32_t j = 0; j < h; j++) {
            uint8_t *p = row;
            for (int32_t i = 0; i < w; i++, p += fb_bypp) fb_store(p, native);
            row += fb.pitch;
        }
    }
    fb_mark_dirty((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

void fb_draw_hline(int32_t x, int32_t y, int32_t w, fb_color_t c) {
    fb_fill_rect(x, y, w, 1, c);
}

void fb_draw_vline(int32_t x, int32_t y, int32_t h, fb_color_t c) {
    fb_fill_rect(x, y, 1, h, c);
}

void fb_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, fb_color_t c) {
    if (w <= 0 || h <= 0) return;
    fb_draw_hline(x, y, w, c);
    fb_draw_hline(x, y + h - 1, w, c);
    fb_draw_vline(x, y, h, c);
    fb_draw_vline(x + w - 1, y, h, c);
}

void fb_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, fb_color_t c) {
    // Bresenham; per-pixel clipping keeps it simple and safe.
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy;

    for (;;) {
        if (x0 >= 0 && y0 >= 0) fb_put_pixel_c((uint32_t)x0, (uint32_t)y0, c);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void fb_draw_circle(int32_t cx, int32_t cy, int32_t radius, fb_color_t c) {
    if (radius <= 0) return;
    int32_t x = radius, y = 0, err = 1 - radius;
    while (x >= y) {
        fb_put_pixel_c((uint32_t)(cx + x), (uint32_t)(cy + y), c);
        fb_put_pixel_c((uint32_t)(cx + y), (uint32_t)(cy + x), c);
        fb_put_pixel_c((uint32_t)(cx - y), (uint32_t)(cy + x), c);
        fb_put_pixel_c((uint32_t)(cx - x), (uint32_t)(cy + y), c);
        fb_put_pixel_c((uint32_t)(cx - x), (uint32_t)(cy - y), c);
        fb_put_pixel_c((uint32_t)(cx - y), (uint32_t)(cy - x), c);
        fb_put_pixel_c((uint32_t)(cx + y), (uint32_t)(cy - x), c);
        fb_put_pixel_c((uint32_t)(cx + x), (uint32_t)(cy - y), c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void fb_fill_circle(int32_t cx, int32_t cy, int32_t radius, fb_color_t c) {
    if (radius <= 0) return;
    for (int32_t dy = -radius; dy <= radius; dy++) {
        // Horizontal span at this row: dx^2 <= r^2 - dy^2.
        int32_t rem = radius * radius - dy * dy;
        int32_t dx = 0;
        while ((dx + 1) * (dx + 1) <= rem) dx++;
        fb_draw_hline(cx - dx, cy + dy, 2 * dx + 1, c);
    }
}

void fb_blit(int32_t x, int32_t y, int32_t w, int32_t h,
             const uint32_t *argb, uint32_t stride) {
    int32_t cx = x, cy = y, cw = w, ch = h;
    if (!fb_clip(&cx, &cy, &cw, &ch)) return;
    // Offset into the source for the clipped-away top/left.
    const uint32_t *src = argb + (uint64_t)(cy - y) * stride + (cx - x);

    uint8_t *row = fb_pixel_ptr(fb_draw, (uint32_t)cx, (uint32_t)cy);
    for (int32_t j = 0; j < ch; j++) {
        if (fb.bpp == 32 && fb.format == FB_FORMAT_BGR) {
            // Native layout matches 0x00RRGGBB stores directly.
            uint32_t *p = (uint32_t *)row;
            for (int32_t i = 0; i < cw; i++) p[i] = src[i] & 0x00FFFFFFU;
        } else {
            uint8_t *p = row;
            for (int32_t i = 0; i < cw; i++, p += fb_bypp)
                fb_store(p, fb_pack(src[i]));
        }
        src += stride;
        row += fb.pitch;
    }
    fb_mark_dirty((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch);
}

void fb_blit_blend(int32_t x, int32_t y, int32_t w, int32_t h,
                   const uint32_t *argb, uint32_t stride) {
    int32_t cx = x, cy = y, cw = w, ch = h;
    if (!fb_clip(&cx, &cy, &cw, &ch)) return;
    const uint32_t *src = argb + (uint64_t)(cy - y) * stride + (cx - x);

    uint8_t *row = fb_pixel_ptr(fb_draw, (uint32_t)cx, (uint32_t)cy);
    for (int32_t j = 0; j < ch; j++) {
        uint8_t *p = row;
        for (int32_t i = 0; i < cw; i++, p += fb_bypp) {
            uint32_t s = src[i];
            uint32_t a = s >> 24;
            if (a == 0) continue;
            if (a == 255) { fb_store(p, fb_pack(s)); continue; }
            fb_color_t d = fb_unpack(fb_load(p));
            uint32_t sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
            uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            uint32_t r = (sr * a + dr * (255 - a)) / 255;
            uint32_t g = (sg * a + dg * (255 - a)) / 255;
            uint32_t b = (sb * a + db * (255 - a)) / 255;
            fb_store(p, fb_pack(FB_RGB(r, g, b)));
        }
        src += stride;
        row += fb.pitch;
    }
    fb_mark_dirty((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch);
}

// PREMULTIPLIED per-pixel source-over: dst = src + dst*(255-a)/255 (src channels
// already scaled by a). Opaque (a==255) copies, a==0 skips. The compositor uses
// this to lay the DESKTOP window (home, which renders a translucent scrim over a
// transparent background) onto the aurora, so the aurora shows through it.
void fb_blit_over(int32_t x, int32_t y, int32_t w, int32_t h,
                  const uint32_t *argb, uint32_t stride) {
    int32_t cx = x, cy = y, cw = w, ch = h;
    if (!fb_clip(&cx, &cy, &cw, &ch)) return;
    const uint32_t *src = argb + (uint64_t)(cy - y) * stride + (cx - x);
    uint8_t *row = fb_pixel_ptr(fb_draw, (uint32_t)cx, (uint32_t)cy);
    for (int32_t j = 0; j < ch; j++) {
        uint8_t *p = row;
        for (int32_t i = 0; i < cw; i++, p += fb_bypp) {
            uint32_t s = src[i], a = s >> 24;
            if (a == 0) continue;
            if (a == 255) { fb_store(p, fb_pack(s & 0x00FFFFFFu)); continue; }
            uint32_t inv = 255 - a;
            fb_color_t d = fb_unpack(fb_load(p));
            uint32_t r = ((s >> 16) & 0xFF) + (((d >> 16) & 0xFF) * inv) / 255;
            uint32_t g = ((s >>  8) & 0xFF) + (((d >>  8) & 0xFF) * inv) / 255;
            uint32_t b = ( s        & 0xFF) + (( d        & 0xFF) * inv) / 255;
            if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
            fb_store(p, fb_pack(FB_RGB(r, g, b)));
        }
        src += stride;
        row += fb.pitch;
    }
    fb_mark_dirty((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch);
}

// Blit a rectangle with a UNIFORM window alpha over the destination:
// dst = (src*alpha + dst*(255-alpha))/255. The compositor uses this to lay a
// GLASS window (rendered opaque by the app) over an already-blurred backdrop,
// so the whole window -- title bar and content gaps -- shows the frosted
// desktop through it. alpha>=255 degenerates to a plain copy.
void fb_blit_uniform(int32_t x, int32_t y, int32_t w, int32_t h,
                     const uint32_t *argb, uint32_t stride, uint32_t alpha) {
    if (alpha >= 255) { fb_blit(x, y, w, h, argb, stride); return; }
    int32_t cx = x, cy = y, cw = w, ch = h;
    if (!fb_clip(&cx, &cy, &cw, &ch)) return;
    uint32_t inv = 255 - alpha;
    const uint32_t *src = argb + (uint64_t)(cy - y) * stride + (cx - x);
    uint8_t *row = fb_pixel_ptr(fb_draw, (uint32_t)cx, (uint32_t)cy);
    for (int32_t j = 0; j < ch; j++) {
        uint8_t *p = row;
        for (int32_t i = 0; i < cw; i++, p += fb_bypp) {
            uint32_t s = src[i];
            fb_color_t d = fb_unpack(fb_load(p));
            uint32_t r = (((s >> 16) & 0xFF) * alpha + ((d >> 16) & 0xFF) * inv) / 255;
            uint32_t g = (((s >>  8) & 0xFF) * alpha + ((d >>  8) & 0xFF) * inv) / 255;
            uint32_t b = (( s        & 0xFF) * alpha + ( d        & 0xFF) * inv) / 255;
            fb_store(p, fb_pack(FB_RGB(r, g, b)));
        }
        src += stride;
        row += fb.pitch;
    }
    fb_mark_dirty((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch);
}

// In-place separable box blur of an fb_draw region (the backdrop behind a glass
// window). Two passes (H then V) with a rolling per-channel sum ~= a small
// Gaussian. Clamped-edge sampling. Silently no-ops if scratch can't be had.
void fb_blur_region(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius) {
    if (radius < 1) return;
    int32_t cx = x, cy = y, cw = w, ch = h;
    if (!fb_clip(&cx, &cy, &cw, &ch)) return;
    if (cw <= 1 || ch <= 1) return;
    uint32_t *a = (uint32_t *)kmalloc((size_t)cw * ch * 4);
    if (!a) return;
    uint32_t *b = (uint32_t *)kmalloc((size_t)cw * ch * 4);
    if (!b) { kfree(a); return; }
    for (int32_t j = 0; j < ch; j++) {
        uint8_t *p = fb_pixel_ptr(fb_draw, (uint32_t)cx, (uint32_t)(cy + j));
        for (int32_t i = 0; i < cw; i++, p += fb_bypp) a[j * cw + i] = fb_unpack(fb_load(p)) & 0xFFFFFFu;
    }
    int32_t win = 2 * radius + 1;
    for (int32_t j = 0; j < ch; j++) {                 /* horizontal a -> b */
        uint32_t *ra = &a[j * cw], *rb = &b[j * cw];
        int32_t sr = 0, sg = 0, sb = 0;
        for (int32_t k = -radius; k <= radius; k++) {
            uint32_t v = ra[k < 0 ? 0 : (k >= cw ? cw - 1 : k)];
            sr += (v >> 16) & 0xFF; sg += (v >> 8) & 0xFF; sb += v & 0xFF;
        }
        for (int32_t i = 0; i < cw; i++) {
            rb[i] = FB_RGB(sr / win, sg / win, sb / win);
            int32_t o = i - radius, n = i + radius + 1;
            uint32_t vo = ra[o < 0 ? 0 : o], vn = ra[n >= cw ? cw - 1 : n];
            sr += ((vn >> 16) & 0xFF) - ((vo >> 16) & 0xFF);
            sg += ((vn >>  8) & 0xFF) - ((vo >>  8) & 0xFF);
            sb += ( vn        & 0xFF) - ( vo        & 0xFF);
        }
    }
    for (int32_t i = 0; i < cw; i++) {                 /* vertical b -> a */
        int32_t sr = 0, sg = 0, sb = 0;
        for (int32_t k = -radius; k <= radius; k++) {
            uint32_t v = b[(k < 0 ? 0 : (k >= ch ? ch - 1 : k)) * cw + i];
            sr += (v >> 16) & 0xFF; sg += (v >> 8) & 0xFF; sb += v & 0xFF;
        }
        for (int32_t j = 0; j < ch; j++) {
            a[j * cw + i] = FB_RGB(sr / win, sg / win, sb / win);
            int32_t o = j - radius, n = j + radius + 1;
            uint32_t vo = b[(o < 0 ? 0 : o) * cw + i], vn = b[(n >= ch ? ch - 1 : n) * cw + i];
            sr += ((vn >> 16) & 0xFF) - ((vo >> 16) & 0xFF);
            sg += ((vn >>  8) & 0xFF) - ((vo >>  8) & 0xFF);
            sb += ( vn        & 0xFF) - ( vo        & 0xFF);
        }
    }
    for (int32_t j = 0; j < ch; j++) {
        uint8_t *p = fb_pixel_ptr(fb_draw, (uint32_t)cx, (uint32_t)(cy + j));
        for (int32_t i = 0; i < cw; i++, p += fb_bypp) fb_store(p, fb_pack(a[j * cw + i]));
    }
    fb_mark_dirty((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch);
    kfree(b); kfree(a);
}

// Soft accent GLOW around the OUTSIDE of a rect [fx0,fy0)-(fx1,fy1): a band of
// width `gw` whose alpha falls off quadratically with distance from the edge,
// blended over the framebuffer. Clipped to the given rect (the caller's repaint
// region) and the screen. Used for the focused-window halo.
void fb_glow_rect(int32_t fx0, int32_t fy0, int32_t fx1, int32_t fy1, int32_t gw,
                  uint8_t cr, uint8_t cg, uint8_t cb, uint8_t max_a,
                  int32_t clx0, int32_t cly0, int32_t clx1, int32_t cly1) {
    if (gw <= 0 || max_a == 0) return;
    int32_t bx0 = fx0 - gw, by0 = fy0 - gw, bx1 = fx1 + gw, by1 = fy1 + gw;
    if (bx0 < clx0) bx0 = clx0; if (by0 < cly0) by0 = cly0;
    if (bx1 > clx1) bx1 = clx1; if (by1 > cly1) by1 = cly1;
    if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0;
    if (bx1 > (int32_t)fb.width)  bx1 = (int32_t)fb.width;
    if (by1 > (int32_t)fb.height) by1 = (int32_t)fb.height;
    int32_t gw2 = gw * gw;
    for (int32_t y = by0; y < by1; y++) {
        uint8_t *row = fb_pixel_ptr(fb_draw, (uint32_t)bx0, (uint32_t)y);
        for (int32_t x = bx0; x < bx1; x++, row += fb_bypp) {
            if (x >= fx0 && x < fx1 && y >= fy0 && y < fy1) continue;  /* window covers */
            int32_t dx = x < fx0 ? fx0 - x : (x >= fx1 ? x - (fx1 - 1) : 0);
            int32_t dy = y < fy0 ? fy0 - y : (y >= fy1 ? y - (fy1 - 1) : 0);
            int32_t d = dx > dy ? dx : dy;
            if (d >= gw) continue;
            int32_t t = gw - d;
            uint32_t ea = (uint32_t)max_a * (uint32_t)(t * t) / (uint32_t)gw2;
            if (!ea) continue;
            uint32_t inv = 255 - ea;
            fb_color_t d0 = fb_unpack(fb_load(row));
            uint32_t r = ((uint32_t)cr * ea + ((d0 >> 16) & 0xFF) * inv) / 255;
            uint32_t g = ((uint32_t)cg * ea + ((d0 >>  8) & 0xFF) * inv) / 255;
            uint32_t b = ((uint32_t)cb * ea + ( d0        & 0xFF) * inv) / 255;
            fb_store(row, fb_pack(FB_RGB(r, g, b)));
        }
    }
    fb_mark_dirty((uint32_t)bx0, (uint32_t)by0, (uint32_t)(bx1 - bx0), (uint32_t)(by1 - by0));
}

void fb_copy_rect(int32_t dst_x, int32_t dst_y,
                  int32_t src_x, int32_t src_y, int32_t w, int32_t h) {
    // Clip both rectangles by the same amount so they stay congruent.
    if (w <= 0 || h <= 0) return;
    int32_t d;
    if (src_x < 0) { d = -src_x; src_x = 0; dst_x += d; w -= d; }
    if (src_y < 0) { d = -src_y; src_y = 0; dst_y += d; h -= d; }
    if (dst_x < 0) { d = -dst_x; dst_x = 0; src_x += d; w -= d; }
    if (dst_y < 0) { d = -dst_y; dst_y = 0; src_y += d; h -= d; }
    if (w <= 0 || h <= 0) return;
    if (src_x + w > (int32_t)fb.width)  w = (int32_t)fb.width  - src_x;
    if (src_y + h > (int32_t)fb.height) h = (int32_t)fb.height - src_y;
    if (dst_x + w > (int32_t)fb.width)  w = (int32_t)fb.width  - dst_x;
    if (dst_y + h > (int32_t)fb.height) h = (int32_t)fb.height - dst_y;
    if (w <= 0 || h <= 0) return;

    uint32_t bytes = (uint32_t)w * fb_bypp;
    if (dst_y <= src_y) {
        for (int32_t j = 0; j < h; j++)
            memmove(fb_pixel_ptr(fb_draw, (uint32_t)dst_x, (uint32_t)(dst_y + j)),
                    fb_pixel_ptr(fb_draw, (uint32_t)src_x, (uint32_t)(src_y + j)),
                    bytes);
    } else {
        for (int32_t j = h - 1; j >= 0; j--)
            memmove(fb_pixel_ptr(fb_draw, (uint32_t)dst_x, (uint32_t)(dst_y + j)),
                    fb_pixel_ptr(fb_draw, (uint32_t)src_x, (uint32_t)(src_y + j)),
                    bytes);
    }
    fb_mark_dirty((uint32_t)dst_x, (uint32_t)dst_y, (uint32_t)w, (uint32_t)h);
}

void fb_scroll_up(uint32_t pixels, fb_color_t fill) {
    if (pixels == 0) return;
    if (pixels >= fb.height) {
        fb_fill_rect(0, 0, (int32_t)fb.width, (int32_t)fb.height, fill);
        return;
    }
    uint64_t move_bytes = (uint64_t)(fb.height - pixels) * fb.pitch;
    memmove(fb_draw, fb_draw + (uint64_t)pixels * fb.pitch, move_bytes);
    fb_fill_rect(0, (int32_t)(fb.height - pixels),
                 (int32_t)fb.width, (int32_t)pixels, fill);
    fb_mark_dirty(0, 0, fb.width, fb.height);
}

void fb_clear(uint8_t r, uint8_t g, uint8_t b) {
    fb_fill_rect(0, 0, (int32_t)fb.width, (int32_t)fb.height, FB_RGB(r, g, b));
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void fb_draw_char(char c, int32_t px, int32_t py, uint8_t fg_r, uint8_t fg_g, uint8_t fg_b, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    const uint8_t *glyph = &font_8x16[(uint8_t)c * FONT_HEIGHT];
    uint32_t fg = fb_pack(FB_RGB(fg_r, fg_g, fg_b));
    uint32_t bg = fb_pack(FB_RGB(bg_r, bg_g, bg_b));

    // Fully on-screen glyphs take the fast row path; edge cases (incl. NEGATIVE
    // px/py -- a title dragged off the top/left) clip per pixel. The px/py >= 0
    // guard is load-bearing: without it a negative coord would pass the width/
    // height check via unsigned overflow and write far out of the framebuffer.
    if (px >= 0 && py >= 0 &&
        px + FONT_WIDTH <= (int32_t)fb.width && py + FONT_HEIGHT <= (int32_t)fb.height) {
        uint8_t *row = fb_pixel_ptr(fb_draw, (uint32_t)px, (uint32_t)py);
        for (uint32_t r = 0; r < FONT_HEIGHT; r++) {
            uint8_t bits = glyph[r];
            if (fb.bpp == 32) {
                uint32_t *p = (uint32_t *)row;
                for (uint32_t col = 0; col < FONT_WIDTH; col++)
                    p[col] = (bits & (0x80U >> col)) ? fg : bg;
            } else {
                uint8_t *p = row;
                for (uint32_t col = 0; col < FONT_WIDTH; col++, p += fb_bypp)
                    fb_store(p, (bits & (0x80U >> col)) ? fg : bg);
            }
            row += fb.pitch;
        }
        fb_mark_dirty(px, py, FONT_WIDTH, FONT_HEIGHT);
        return;
    }

    for (uint32_t r = 0; r < FONT_HEIGHT; r++) {
        uint8_t bits = glyph[r];
        for (uint32_t col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80U >> col))
                fb_put_pixel(px + col, py + r, fg_r, fg_g, fg_b);
            else
                fb_put_pixel(px + col, py + r, bg_r, bg_g, bg_b);
        }
    }
}

void fb_draw_string(char *str, int32_t px, int32_t py, uint8_t fg_r, uint8_t fg_g, uint8_t fg_b, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    int32_t x = px;
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
    return fb_draw;
}

/* Selftest: draws known primitives, then reads them back via fb_get_pixel
 * and asserts the exact color — a real, falsifiable assertion (not "it
 * didn't crash"), and one that exercises the actual color pack/unpack path
 * for whatever mode is currently active (16/24/32bpp, RGB or BGR).
 *
 * Test colors are pure primaries (each channel either 0x00 or 0xFF) so the
 * round trip is lossless at every supported bit depth, including 16bpp's
 * 5-6-5 truncation — this test is about drawing correctness, not color
 * precision, so precision loss shouldn't be able to fail it.
 *
 * This only covers software drawing correctness. It does not verify actual
 * on-screen/on-host output (Bochs DISPI modeset, VirtIO-GPU accelerated
 * scan-out) — those were verified manually via QEMU screendumps this
 * session; see docs/TODO.md's "no automated selftest for the display or
 * USB stack" entry, which this closes only the software-drawing half of. */
int fb_run_selftests(void) {
    const fb_info_t *info = fb_get_info();
    if (info->width < 32 || info->height < 16) {
        kprintf("fb_run_selftests: resolution too small to test (%ux%u)\n",
                (unsigned int)info->width, (unsigned int)info->height);
        return -1;
    }

    bool ok = true;

    // Two adjacent 16x16 squares, red then green — also checks the boundary
    // column on each side doesn't bleed into the other.
    fb_fill_rect(0, 0, 16, 16, FB_RGB(255, 0, 0));
    fb_fill_rect(16, 0, 16, 16, FB_RGB(0, 255, 0));

    ok = ok && (fb_get_pixel(5, 5)   == FB_RGB(255, 0, 0));
    ok = ok && (fb_get_pixel(15, 5)  == FB_RGB(255, 0, 0));   // last red column
    ok = ok && (fb_get_pixel(16, 5)  == FB_RGB(0, 255, 0));   // first green column
    ok = ok && (fb_get_pixel(20, 5)  == FB_RGB(0, 255, 0));

    // fb_copy_rect: overwrite the red square with a copy of the green one.
    fb_copy_rect(0, 0, 16, 0, 16, 16);
    ok = ok && (fb_get_pixel(5, 5) == FB_RGB(0, 255, 0));

    // Restore the touched area (cosmetic best-effort, not part of the test).
    fb_fill_rect(0, 0, 32, 16, FB_RGB(0, 0, 0));
    fb_present();

    kprintf("fb_run_selftests: fill_rect/get_pixel/copy_rect round-trip %s\n",
            ok ? "OK" : "FAIL");
    return ok ? 0 : -1;
}
