#ifndef __EMBLINK_UI_RENDER_TARGET_H__
#define __EMBLINK_UI_RENDER_TARGET_H__

/* ui/backend/render_target.h -- EmbLink UI Piece 4a: the surface every
 * backend call draws into.
 *
 * A render_target wraps a raw pixel buffer plus its geometry. Three concrete
 * sources use the SAME struct (Section 1):
 *   - a mapped Piece-1 surface buffer (a client drawing its own window),
 *   - the real framebuffer / gpu_driver resource (the compositor's output),
 *   - a scratch-pool buffer (opacity groups Section 5, backdrop blur Section 4).
 *
 * Internal targets are always premultiplied BGRA8888 (Piece 1 Section 0); the
 * ONLY format-conversion point is the final blit to whatever struct gpu_driver
 * wants -- which is gpu_driver's job, not this piece's. */

#include <stdint.h>
#include "scene.h"   /* for enum embk_pixfmt */

struct render_target {
    void    *pixels;      /* base address of the top-left pixel */
    uint32_t width, height;
    uint32_t stride;       /* bytes per row (>= width*4) */
    enum embk_pixfmt format;
};

#endif /* __EMBLINK_UI_RENDER_TARGET_H__ */
