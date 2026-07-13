/* ui/kit/kit.c -- the themed widget kit (see kit.h). Everything here composes
 * ui/declare primitives and pulls every dimension/colour from ui/theme. */

#include "kit.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TH (ui_theme())

static struct layout_size sz_fixed(float v) { return (struct layout_size){ SIZE_FIXED, v, 0, 0, 0 }; }
static struct layout_size sz_grow(void)     { return (struct layout_size){ SIZE_FLEX, 0, 1, 0, 0 }; }
static struct layout_size sz_flex(float w)  { return (struct layout_size){ SIZE_FLEX, 0, w, 0, 0 }; }
static struct layout_size sz_intrinsic(void){ return (struct layout_size){ SIZE_INTRINSIC, 0, 0, 0, 0 }; }

static struct paint solid(struct color c) {
    struct paint p; p.kind = PAINT_SOLID; p.solid = c; p.n_stops = 0; return p;
}

/* --- structure ---------------------------------------------------------- */

void ui_screen_begin(void) {
    ui_begin_vstack(0);
    const struct ui_theme *t = TH;
    ui_set_paint(solid(t->bg));
    ui_set_size(sz_grow(), sz_grow());
    ui_set_padding(t->sp6, t->sp6, t->sp6, t->sp6);
    ui_set_spacing(t->sp5);
}
void ui_screen_end(void) { ui_end_stack(); }

void ui_card_begin(uint64_t key) {
    ui_box_begin(key);
    const struct ui_theme *t = TH;
    ui_set_paint(solid(t->surface));
    ui_set_corner_radius(t->radius_lg);
    ui_set_border(1.0f, t->border);
    ui_set_shadow(true, t->shadow_md.dx, t->shadow_md.dy, t->shadow_md.blur, t->shadow_md.color);
    ui_set_padding(t->sp5, t->sp5, t->sp5, t->sp5);
    ui_set_spacing(t->sp4);
    ui_set_size(sz_intrinsic(), sz_intrinsic());
    ui_set_axis(AXIS_COLUMN);   /* card is a column */
    ui_set_align(ALIGN_STRETCH);
}
void ui_card_end(void) { ui_box_end(); }

void ui_panel_begin(uint64_t key) {
    ui_box_begin(key);
    const struct ui_theme *t = TH;
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_md);
    ui_set_border(1.0f, t->border);
    ui_set_padding(t->sp4, t->sp4, t->sp4, t->sp4);
    ui_set_spacing(t->sp3);
    ui_set_axis(AXIS_COLUMN);
    ui_set_align(ALIGN_STRETCH);
}
void ui_panel_end(void) { ui_box_end(); }

void ui_row_begin(uint64_t key) {
    ui_begin_hstack(key);
    ui_set_spacing(TH->sp3);
    ui_set_align(ALIGN_CENTER);
}
void ui_row_end(void) { ui_end_stack(); }
void ui_col_begin(uint64_t key) { ui_begin_vstack(key); ui_set_spacing(TH->sp2); }
void ui_col_end(void) { ui_end_stack(); }

void ui_divider(void) {
    const struct ui_theme *t = TH;
    ui_box_begin(0);
    ui_set_paint(solid(t->border));
    ui_set_size(sz_grow(), sz_fixed(1));
    ui_box_end();
}
void ui_flex_spacer(void) { ui_spacer(); }
void ui_gap(float px) {
    ui_box_begin(0);
    ui_set_size(sz_fixed(px), sz_fixed(px));
    ui_box_end();
}

/* --- typography --------------------------------------------------------- */

static void text_role(uint32_t font, float size, struct color color, const char *s) {
    ui_set_font(font);
    ui_set_text_size(size);
    ui_set_text_color(color);
    ui_text("%s", s);
}
#define FMT(buf) do { va_list ap; va_start(ap, fmt); vsnprintf((buf), sizeof(buf), fmt, ap); va_end(ap); } while (0)

void ui_heading(const char *fmt, ...) { char b[256]; FMT(b); const struct ui_theme *t=TH; text_role(t->font_bold, t->text_heading, t->text, b); }
void ui_title(const char *fmt, ...)   { char b[256]; FMT(b); const struct ui_theme *t=TH; text_role(t->font_bold, t->text_title, t->text, b); }
void ui_body(const char *fmt, ...)    { char b[256]; FMT(b); const struct ui_theme *t=TH; text_role(t->font_regular, t->text_body, t->text, b); }
void ui_secondary(const char *fmt, ...){ char b[256]; FMT(b); const struct ui_theme *t=TH; text_role(t->font_regular, t->text_body, t->text_secondary, b); }
void ui_caption(const char *fmt, ...) { char b[256]; FMT(b); const struct ui_theme *t=TH; text_role(t->font_regular, t->text_caption, t->text_tertiary, b); }

/* --- buttons ------------------------------------------------------------ */

/* Scale a colour's brightness (k>1 lighter, k<1 darker) for hover/press feedback. */
static struct color shade(struct color c, float k) {
    struct color o = { c.r * k, c.g * k, c.b * k, c.a };
    if (o.r > 1) o.r = 1;
    if (o.g > 1) o.g = 1;
    if (o.b > 1) o.b = 1;
    return o;
}

static bool button_impl(const char *label, struct color fill, struct color text_c,
                        float border_w, struct color border_c, bool has_fill) {
    const struct ui_theme *t = TH;
    ui_box_begin(0);
    struct instance_handle self = ui_open();
    bool pressed = ui_is_pressed(), hovered = ui_is_hovered();
    if (has_fill) {
        struct color f = pressed ? shade(fill, 0.86f) : hovered ? shade(fill, 1.10f) : fill;
        ui_set_paint(solid(f));
    } else if (hovered) {                 /* ghost: soft wash on hover */
        ui_set_paint(solid(shade(t->accent_soft, pressed ? 0.9f : 1.0f)));
    }
    ui_set_corner_radius(t->radius_md);
    if (border_w > 0) ui_set_border(border_w, hovered ? t->accent : border_c);
    ui_set_padding(t->sp2 + 1, t->sp4, t->sp2 + 1, t->sp4);
    ui_set_align(ALIGN_CENTER);
    text_role(t->font_bold, t->text_body, text_c, label);
    ui_box_end();
    return ui_consume_click(self);
}
bool ui_button_primary(const char *label) {
    const struct ui_theme *t = TH;
    return button_impl(label, t->accent, t->on_accent, 0, t->accent, true);
}
bool ui_button_secondary(const char *label) {
    const struct ui_theme *t = TH;
    return button_impl(label, t->surface, t->text, 1.0f, t->border_strong, true);
}
bool ui_button_ghost(const char *label) {
    const struct ui_theme *t = TH;
    return button_impl(label, t->accent, t->accent, 0, t->accent, false);
}

/* --- toggle ------------------------------------------------------------- */

bool ui_toggle(bool on) {
    const struct ui_theme *t = TH;
    /* track: a pill, accent when on / surface_alt when off, knob pushed to the
     * far end via justify (END when on, START when off) -- pure flex, no
     * absolute positioning. */
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    struct color track = on ? t->accent : t->border_strong;
    if (ui_is_hovered()) track = shade(track, 1.12f);   /* brighten on hover */
    ui_set_paint(solid(track));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_fixed(42), sz_fixed(24));
    ui_set_padding(3, 3, 3, 3);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(on ? JUSTIFY_END : JUSTIFY_START);
    /* knob */
    ui_box_begin(0);
    ui_set_paint(solid(t->on_accent));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_fixed(18), sz_fixed(18));
    ui_box_end();
    ui_end_stack();
    return ui_consume_click(self);
}

/* --- checkbox / radio --------------------------------------------------- */

bool ui_checkbox(bool on) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), press = ui_is_pressed();
    struct color fill = on ? (press ? shade(t->accent, 0.86f) : t->accent)
                           : (hov  ? shade(t->surface_alt, 1.15f) : t->surface_alt);
    ui_set_paint(solid(fill));
    ui_set_corner_radius(t->radius_sm);
    ui_set_size(sz_fixed(20), sz_fixed(20));
    if (!on) ui_set_border(1.0f, hov ? t->accent : t->border_strong);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    if (on) text_role(t->font_bold, t->text_caption, t->on_accent, "\xE2\x9C\x93");  /* U+2713 check */
    ui_end_stack();
    return ui_consume_click(self);
}

bool ui_radio(bool selected) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered();
    ui_set_paint(solid(selected ? t->accent_soft : t->surface_alt));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_fixed(20), sz_fixed(20));
    ui_set_border(1.5f, selected ? t->accent : (hov ? t->accent : t->border_strong));
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    if (selected) {
        ui_box_begin(0);
        ui_set_paint(solid(t->accent));
        ui_set_corner_radius(t->radius_pill);
        ui_set_size(sz_fixed(10), sz_fixed(10));
        ui_box_end();
    }
    ui_end_stack();
    return ui_consume_click(self);
}

/* --- progress / slider -------------------------------------------------- */

/* A pill track with an accent-filled leading portion. Fill width is `frac` of
 * the track via flex weights, so it works at any track width (no absolute pos). */
void ui_progress(float frac) {
    const struct ui_theme *t = TH;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    ui_begin_hstack(0);
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_grow(), sz_fixed(8));
    ui_set_align(ALIGN_STRETCH);
    ui_box_begin(0);                          /* filled leading portion */
    ui_set_paint(solid(t->accent));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_flex(frac > 0.001f ? frac : 0.001f), sz_grow());
    ui_box_end();
    ui_box_begin(0);                          /* remainder (transparent) */
    ui_set_size(sz_flex(1.0f - frac + 0.001f), sz_fixed(1));
    ui_box_end();
    ui_end_stack();
}

/* Drag anywhere on the full-height hit strip; the pointer maps into the track's
 * world rect (captured, so the drag continues even off the strip). */
float ui_slider(float value) {
    const struct ui_theme *t = TH;
    if (value < 0) value = 0;
    if (value > 1) value = 1;
    ui_begin_hstack(0);
    (void)ui_open();
    float out = value;
    if (ui_is_active()) {
        float rx, ry, rw, rh, px, py;
        if (ui_open_rect(&rx, &ry, &rw, &rh) && rw > 0) {
            ui_pointer_pos(&px, &py);
            out = (px - rx) / rw;
            if (out < 0) out = 0;
            if (out > 1) out = 1;
        }
    }
    ui_set_size(sz_grow(), sz_fixed(24));     /* tall hit strip */
    ui_set_align(ALIGN_CENTER);
    ui_box_begin(0);                          /* filled leading portion */
    ui_set_paint(solid(t->accent));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_flex(value > 0.001f ? value : 0.001f), sz_fixed(4));
    ui_box_end();
    ui_box_begin(0);                          /* knob */
    ui_set_paint(solid(t->on_accent));
    ui_set_corner_radius(t->radius_pill);
    ui_set_border(1.0f, ui_is_hovered() || ui_is_active() ? t->accent : t->border_strong);
    ui_set_size(sz_fixed(18), sz_fixed(18));
    ui_box_end();
    ui_box_begin(0);                          /* remainder track */
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_flex(1.0f - value + 0.001f), sz_fixed(4));
    ui_box_end();
    ui_end_stack();
    return out;
}

/* --- segmented control (tabs) ------------------------------------------- */

int ui_segmented(const char *const *labels, int count, int selected) {
    const struct ui_theme *t = TH;
    int result = selected;
    ui_begin_hstack(0);
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_md);
    ui_set_padding(3, 3, 3, 3);
    ui_set_spacing(2);
    for (int i = 0; i < count; i++) {
        ui_box_begin((uint64_t)(i + 1));
        struct instance_handle seg = ui_open();
        bool sel = (i == selected);
        if (sel) {
            ui_set_paint(solid(t->surface));
            ui_set_shadow(true, t->shadow_sm.dx, t->shadow_sm.dy, t->shadow_sm.blur, t->shadow_sm.color);
        } else if (ui_is_hovered()) {
            ui_set_paint(solid(shade(t->surface_alt, 1.10f)));
        }
        ui_set_corner_radius(t->radius_sm);
        ui_set_padding(t->sp1, t->sp3, t->sp1, t->sp3);
        ui_set_align(ALIGN_CENTER);
        ui_set_justify(JUSTIFY_CENTER);
        text_role(t->font_bold, t->text_body, sel ? t->text : t->text_secondary, labels[i]);
        ui_box_end();
        if (ui_consume_click(seg)) result = i;
    }
    ui_end_stack();
    return result;
}

/* --- chip / avatar ------------------------------------------------------ */

bool ui_chip(const char *label, bool active) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered();
    struct color bg = active ? t->accent_soft : (hov ? shade(t->surface_alt, 1.12f) : t->surface_alt);
    ui_set_paint(solid(bg));
    ui_set_corner_radius(t->radius_pill);
    ui_set_border(1.0f, active ? t->accent : t->border);
    ui_set_padding(t->sp1, t->sp3, t->sp1, t->sp3);
    ui_set_align(ALIGN_CENTER);
    text_role(t->font_bold, t->text_caption, active ? t->accent : t->text_secondary, label);
    ui_end_stack();
    return ui_consume_click(self);
}

void ui_avatar(const char *initials) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    ui_set_paint(solid(t->accent_soft));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_fixed(36), sz_fixed(36));
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    text_role(t->font_bold, t->text_body, t->accent, initials);
    ui_end_stack();
}

/* --- text field --------------------------------------------------------- */

bool ui_text_field(char *buf, unsigned long cap, const char *placeholder) {
    const struct ui_theme *t = TH;
    ui_box_begin(0);
    struct instance_handle self = ui_open();
    if (ui_consume_click(self)) ui_request_focus(self);
    bool focused = ui_has_focus(self);

    /* while focused, apply this frame's typed characters in place */
    if (focused) {
        char in[32];
        int n = ui_input_take(in, (int)sizeof in);
        unsigned long len = strlen(buf);
        for (int i = 0; i < n; i++) {
            char c = in[i];
            if (c == '\b') { if (len > 0) buf[--len] = 0; }
            else if (c == '\n' || c == '\t') { /* single-line: ignore submit/tab */ }
            else if ((unsigned char)c >= 32 && (unsigned char)c < 127 && len + 1 < cap) {
                buf[len++] = c; buf[len] = 0;
            }
        }
    }

    /* the input well */
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_md);
    ui_set_border(focused ? 1.5f : 1.0f, focused ? t->accent : t->border_strong);
    ui_set_padding(t->sp2 + 1, t->sp3, t->sp2 + 1, t->sp3);
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_set_align(ALIGN_CENTER);

    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(1);
    if (buf[0] == 0 && !focused) {
        text_role(t->font_regular, t->text_body, t->text_tertiary, placeholder);
    } else {
        text_role(t->font_regular, t->text_body, t->text, buf);
        if (focused) {                    /* caret */
            ui_box_begin(0);
            ui_set_paint(solid(t->accent));
            ui_set_size(sz_fixed(2), sz_fixed(t->text_body));
            ui_box_end();
        }
        ui_flex_spacer();                 /* keep text left-aligned in the well */
    }
    ui_end_stack();
    ui_box_end();
    return focused;
}

/* --- scroll view -------------------------------------------------------- */

static float *g_scroll_ptr;

void ui_scroll_begin(uint64_t key, float viewport_h, float *scroll_y) {
    const struct ui_theme *t = TH;
    ui_begin_vstack(key);
    /* wheel over the view scrolls it (40px per notch; wheel up -> content up) */
    float w = ui_take_wheel();
    if (w != 0.0f) *scroll_y -= w * 40.0f;
    if (*scroll_y < 0) *scroll_y = 0;
    ui_set_size(sz_grow(), sz_fixed(viewport_h));
    ui_set_clip_children(true);
    ui_set_scroll_offset(*scroll_y);
    ui_set_spacing(t->sp2);
    ui_set_align(ALIGN_STRETCH);
    g_scroll_ptr = scroll_y;
}
void ui_scroll_end(void) {
    /* clamp to content using last frame's measured extents (one-frame lag is ok) */
    float content = 0, viewport = 0;
    if (ui_open_content_extent(&content, &viewport)) {
        float maxs = content - viewport;
        if (maxs < 0) maxs = 0;
        if (g_scroll_ptr && *g_scroll_ptr > maxs) { *g_scroll_ptr = maxs; ui_set_scroll_offset(maxs); }
    }
    ui_end_stack();
}

/* --- overlay / modal ---------------------------------------------------- */

static struct instance_handle g_overlay_h, g_dialog_h;

void ui_overlay_begin(uint64_t key) {
    ui_begin_hstack(key);
    g_overlay_h = ui_open();
    struct color scrim = { 0.0f, 0.0f, 0.0f, 0.55f };   /* dim the content behind */
    ui_set_overlay(true);                 /* fill the screen, out of flow (paints on top) */
    ui_set_paint(solid(scrim));
    ui_set_size(sz_grow(), sz_grow());
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
}
bool ui_overlay_end(void) {
    ui_end_stack();
    return ui_consume_click(g_overlay_h);   /* true only if the bare scrim was clicked */
}
void ui_dialog_begin(uint64_t key) {
    ui_card_begin(key);
    g_dialog_h = ui_open();
}
void ui_dialog_end(void) {
    ui_consume_click(g_dialog_h);           /* absorb clicks on the dialog so they don't dismiss */
    ui_card_end();
}

/* --- badge -------------------------------------------------------------- */

void ui_badge(const char *label, enum ui_badge_tone tone) {
    const struct ui_theme *t = TH;
    struct color fg, bg;
    switch (tone) {
        case BADGE_SUCCESS: fg = t->success; break;
        case BADGE_WARNING: fg = t->warning; break;
        case BADGE_DANGER:  fg = t->danger;  break;
        case BADGE_NEUTRAL: fg = t->text_secondary; break;
        case BADGE_ACCENT:
        default:            fg = t->accent; break;
    }
    bg = t->accent_soft;
    if (tone == BADGE_NEUTRAL) bg = t->surface_alt;
    ui_begin_hstack(0);
    ui_set_paint(solid(bg));
    ui_set_corner_radius(t->radius_pill);
    ui_set_padding(t->sp1, t->sp2 + 2, t->sp1, t->sp2 + 2);
    ui_set_align(ALIGN_CENTER);
    text_role(t->font_bold, t->text_caption, fg, label);
    ui_end_stack();
}
