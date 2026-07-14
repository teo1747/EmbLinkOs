/* ui/dsl/em.c -- EmUI V2 implementation.
 *
 * Two layers:
 *   1. Containers (VStack/Card/...) emit immediately as brace scopes.
 *   2. Leaves (Text/Button/...) are CHAINABLE: each stages a "pending" element
 *      and returns a vtable of modifier function pointers; the modifiers mutate
 *      the pending element's props; it is emitted ("flushed") at the next
 *      element/container boundary. Deferring the emit is what lets a chain like
 *      Text("Hi").caption().secondary() apply style BEFORE the node is built,
 *      so it composes cleanly with the dirty-rect renderer (no re-style churn). */

#include "em.h"
#include "kit.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TH (ui_theme())

/* ---- small helpers ----------------------------------------------------- */

static struct paint solid(Color c) {
    struct paint p; p.kind = PAINT_SOLID; p.solid = c; p.n_stops = 0; return p;
}
static Color shade(Color c, float k) {
    Color o = { c.r * k, c.g * k, c.b * k, c.a };
    if (o.r > 1) o.r = 1;
    if (o.g > 1) o.g = 1;
    if (o.b > 1) o.b = 1;
    return o;
}
static Color tint(Color c, float a) { Color o = c; o.a = a; return o; }

static struct layout_size sz_fixed(float v)  { return (struct layout_size){ SIZE_FIXED, v, 0, 0, 0 }; }
static struct layout_size sz_grow(void)      { return (struct layout_size){ SIZE_FLEX, 0, 1, 0, 0 }; }
static struct layout_size sz_intrinsic(void) { return (struct layout_size){ SIZE_INTRINSIC, 0, 0, 0, 0 }; }

static void utf8_enc(int cp, char *out) {
    unsigned c = (unsigned)cp;
    if (c < 0x80) { out[0] = (char)c; out[1] = 0; }
    else if (c < 0x800) { out[0] = 0xC0 | (c >> 6); out[1] = 0x80 | (c & 0x3F); out[2] = 0; }
    else if (c < 0x10000) { out[0] = 0xE0 | (c >> 12); out[1] = 0x80 | ((c >> 6) & 0x3F); out[2] = 0x80 | (c & 0x3F); out[3] = 0; }
    else { out[0] = 0xF0 | (c >> 18); out[1] = 0x80 | ((c >> 12) & 0x3F); out[2] = 0x80 | ((c >> 6) & 0x3F); out[3] = 0x80 | (c & 0x3F); out[4] = 0; }
}

static void em_resolve_font(EmFont role, uint32_t *fh, float *sz) {
    const struct ui_theme *t = TH;
    switch (role) {
        case Title:    *fh = t->font_bold;    *sz = t->text_title;   break;
        case Heading:  *fh = t->font_bold;    *sz = t->text_heading; break;
        case Caption:  *fh = t->font_regular; *sz = t->text_caption; break;
        case BodyBold: *fh = t->font_bold;    *sz = t->text_body;    break;
        default:       *fh = t->font_regular; *sz = t->text_body;    break;
    }
}
static enum layout_align  map_align(EmAlign a) {
    switch (a) { case Leading: return ALIGN_START; case Center: return ALIGN_CENTER;
                 case Trailing: return ALIGN_END; case Fill: return ALIGN_STRETCH; default: return ALIGN_START; }
}
static enum layout_justify map_justify(EmAlign a) {
    switch (a) { case Leading: return JUSTIFY_START; case Center: return JUSTIFY_CENTER;
                 case Trailing: return JUSTIFY_END; case SpaceBetween: return JUSTIFY_SPACE_BETWEEN; default: return JUSTIFY_START; }
}

/* True when the active theme is dark (glass tint/edge differ by ground). */
static int em_theme_is_dark(void) {
    Color b = TH->bg;
    return (0.299f * b.r + 0.587f * b.g + 0.114f * b.b) < 0.5f;
}

/* The GLASS material: blur whatever is behind this box, lay a translucent tint
 * over the blur (theme surface nudged toward the EmbLink accent so it reads as
 * ours, not neutral frosted glass), and rim it with a light edge highlight for
 * depth. The tint is <1.0 alpha so it takes cpu_draw_rect's constant-alpha
 * integer-LUT fast path; the blur is the only costly part and, because the
 * app runs a retained loop, a static panel over a static backdrop blurs once
 * and then idles. */
static void em_glass_apply(float blur) {
    const struct ui_theme *t = TH;
    int dark = em_theme_is_dark();
    Color base = t->surface_alt, acc = t->accent;
    Color tint = { base.r * 0.90f + acc.r * 0.10f,
                   base.g * 0.90f + acc.g * 0.10f,
                   base.b * 0.90f + acc.b * 0.10f,
                   dark ? 0.55f : 0.66f };
    Color edge = dark ? (Color){ 1, 1, 1, 0.18f } : (Color){ 1, 1, 1, 0.85f };
    ui_set_backdrop_blur(true, blur > 0 ? blur : 12.0f);
    ui_set_paint(solid(tint));
    ui_set_border(1.0f, edge);
}

static void em_apply_box(EmProps p) {
    if (p.spacing > 0) ui_set_spacing(p.spacing);
    int any_pad = (p.padding > 0 || p.px > 0 || p.py > 0 || p.pt > 0 || p.pr > 0 || p.pb > 0 || p.pl > 0);
    if (any_pad) {
        float t = p.padding, r = p.padding, b = p.padding, l = p.padding;
        if (p.px > 0) l = r = p.px;
        if (p.py > 0) t = b = p.py;
        if (p.pt > 0) t = p.pt;
        if (p.pr > 0) r = p.pr;
        if (p.pb > 0) b = p.pb;
        if (p.pl > 0) l = p.pl;
        ui_set_padding(t, r, b, l);
    }
    if (p.width > 0 || p.height > 0 || p.grow) {
        struct layout_size w = p.width > 0 ? sz_fixed(p.width) : (p.grow ? sz_grow() : sz_intrinsic());
        struct layout_size h = p.height > 0 ? sz_fixed(p.height) : sz_intrinsic();
        ui_set_size(w, h);
    }
    if (p.background.a > 0) ui_set_paint(solid(p.background));
    if (p.corner > 0)       ui_set_corner_radius(p.corner);
    if (p.border > 0)       ui_set_border(p.border, p.border_color.a > 0 ? p.border_color : TH->border);
    if (p.shadow > 0) {
        const struct ui_theme *t = TH;
        struct ui_shadow_spec s = p.shadow == 1 ? t->shadow_sm : p.shadow == 2 ? t->shadow_md : t->shadow_lg;
        ui_set_shadow(true, s.dx, s.dy, s.blur, s.color);
    } else if (p.shadow < 0) {
        ui_set_shadow(false, 0, 0, 0, (Color){0,0,0,0});
    }
    if (p.glass) em_glass_apply(p.blur);   /* overrides fill+border, adds blur */
    if (p.align)   ui_set_align(map_align(p.align));
    if (p.justify) ui_set_justify(map_justify(p.justify));
    if (p.clip)    ui_set_clip_children(true);
    if (p.opacity > 0.0f && p.opacity < 1.0f) ui_set_opacity(p.opacity);
}

/* ---- tokens ------------------------------------------------------------ */

const EmTokens *em_tokens_(void) {
    static EmTokens tok;
    const struct ui_theme *t = TH;
    tok.accent = t->accent; tok.accent_soft = t->accent_soft; tok.on_accent = t->on_accent;
    tok.text = t->text; tok.secondary = t->text_secondary; tok.tertiary = t->text_tertiary;
    tok.surface = t->surface; tok.surface_alt = t->surface_alt; tok.bg = t->bg;
    tok.border = t->border; tok.border_strong = t->border_strong;
    tok.success = t->success; tok.warning = t->warning; tok.danger = t->danger;
    tok.clear = (Color){ 0, 0, 0, 0 };
    return &tok;
}

/* ---- pending element + emit dispatch (declared up front) --------------- */
void em_flush(void);

/* ---- containers (flush the pending leaf, then open) -------------------- */

void em_vstack_(EmProps p) { em_flush(); ui_begin_vstack(0); em_apply_box(p); }
void em_hstack_(EmProps p) { em_flush(); ui_begin_hstack(0); if (!p.align) ui_set_align(ALIGN_CENTER); em_apply_box(p); }
void em_zstack_(EmProps p) { em_flush(); ui_begin_vstack(0); em_apply_box(p); }
void em_glass_(EmProps p)  { em_flush(); ui_begin_vstack(0); p.glass = 1;
                             if (p.corner <= 0) p.corner = TH->radius_lg;
                             em_apply_box(p); }
void em_row_(EmProps p)    { em_flush(); ui_begin_hstack(0); ui_set_align(ALIGN_CENTER); if (!p.spacing) ui_set_spacing(TH->sp3); em_apply_box(p); }
void em_end_(void)         { em_flush(); ui_end_stack(); }

/* The page-transition transform (opacity/slide), resolved by em_nav each frame.
 * Applied to the PAGE'S OWN root Screen -- em_nav no longer wraps the page in a
 * second full-size box for this (the Screen is already full-size; that wrapper
 * was redundant). 1.0f/0.0f = settled = a guarded no-op on every non-nav Screen. */
static float g_nav_cur_op = 1.0f, g_nav_cur_slide = 0.0f;

void em_screen_(EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    ui_set_paint(solid(p.background.a > 0 ? p.background : t->bg));
    ui_set_size(sz_grow(), sz_grow());
    ui_set_padding(t->sp6, t->sp6, t->sp6, t->sp6);
    ui_set_spacing(t->sp5);
    em_apply_box(p);
    /* carry the active page transition on the Screen itself (see g_nav_cur_*).
     * Applied every frame (incl. the settled 1.0/0.0) so the fade resets cleanly
     * when the transition ends; both setters are guarded so 1.0/0.0 costs nothing. */
    ui_set_opacity(g_nav_cur_op);
    ui_set_offset(g_nav_cur_slide, 0.0f);
}
void em_card_(EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_box_begin(0);
    ui_set_paint(solid(p.background.a > 0 ? p.background : t->surface));
    ui_set_corner_radius(p.corner > 0 ? p.corner : t->radius_lg);
    ui_set_border(1.0f, t->border);
    if (p.shadow >= 0) ui_set_shadow(true, t->shadow_md.dx, t->shadow_md.dy, t->shadow_md.blur, t->shadow_md.color);
    ui_set_padding(t->sp5, t->sp5, t->sp5, t->sp5);
    ui_set_spacing(t->sp4);
    ui_set_axis(AXIS_COLUMN);
    ui_set_align(ALIGN_STRETCH);
    em_apply_box(p);
}
void em_section_(const char *title, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    ui_set_spacing(p.spacing > 0 ? p.spacing : t->sp2);
    ui_set_align(ALIGN_STRETCH);
    em_apply_box(p);
    if (title && title[0]) {
        ui_set_font(t->font_bold); ui_set_text_size(t->text_caption); ui_set_text_color(t->text_tertiary);
        ui_text("%s", title);
    }
}
void em_navbar_(const char *title, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp3);
    em_apply_box(p);
    if (title && title[0]) {
        ui_set_font(t->font_bold); ui_set_text_size(t->text_title); ui_set_text_color(t->text);
        ui_text("%s", title);
    }
    ui_spacer();
}
void em_scroll_(float *scroll_y, float viewport_h, EmProps p) { em_flush(); ui_scroll_begin(0, viewport_h, scroll_y); em_apply_box(p); }
void em_scroll_end_(void) { em_flush(); ui_scroll_end(); }

/* ======================================================================= */
/* the chainable leaf layer                                                */
/* ======================================================================= */

typedef enum { PK_NONE, PK_TEXT, PK_ICON, PK_LABEL, PK_BADGE, PK_TAG, PK_AVATAR,
               PK_BANNER, PK_PROGRESS, PK_BUTTON, PK_ICONBTN, PK_TOGGLE, PK_CHECK,
               PK_SLIDER, PK_STEPPER, PK_FIELD, PK_SEGMENTED, PK_LISTROW,
               PK_CLOSEBTN, PK_SEARCH, PK_SPINNER, PK_DROPDOWN } PKind;

static struct {
    int active; PKind kind; EmProps props; const char *id;
    const char *str, *str2; int cp; void *bind; int lo, hi; float frac;
    const char *const *labels; int count; size_t cap; char *buf;
    bool result;                 /* interactive result after flush */
} P;

#define ID_MAX 64
static struct { const char *id; bool clicked, hovered; } g_ids[ID_MAX];
static int g_id_n;

/* forward: the actual emitters */
static void em_text_impl(const char *s, EmProps p);
static void em_icon_impl(int cp, EmProps p);
static void em_label_impl(int cp, const char *s, EmProps p);
static void em_badge_impl(const char *s, EmProps p);
static void em_tag_impl(const char *s, EmProps p);
static void em_avatar_impl(const char *s, EmProps p);
static void em_banner_impl(int cp, const char *s, EmProps p);
static void em_progress_impl(float frac, EmProps p);
static bool em_button_impl(const char *s, EmProps p, bool *hov);
static bool em_iconbtn_impl(int cp, EmProps p, bool *hov);
static void em_toggle_impl(const char *l, bool *b, EmProps p);
static void em_checkbox_impl(const char *l, bool *b, EmProps p);
static void em_slider_impl(float *b, EmProps p);
static void em_stepper_impl(const char *l, int *b, int lo, int hi, EmProps p);
static bool em_field_impl(char *buf, size_t cap, const char *ph, EmProps p, bool *hov);
static void em_segmented_impl(const char *const *labels, int count, int *b, EmProps p);
static bool em_listrow_impl(int cp, const char *title, const char *value, EmProps p, bool *hov);
static bool em_closebtn_impl(bool *hov);
static bool em_search_impl(char *buf, size_t cap, const char *ph, bool *hov);
static void em_spinner_impl(void);
static bool em_dropdown_impl(const char *const *labels, int count, int *sel, bool *hov);

void em_flush(void) {
    if (!P.active) return;
    PKind k = P.kind; EmProps pr = P.props; const char *id = P.id;
    P.active = 0;                /* clear first: emitters may create nested elements */
    bool clicked = false, hovered = false;
    switch (k) {
        case PK_TEXT:     em_text_impl(P.str, pr); break;
        case PK_ICON:     em_icon_impl(P.cp, pr); break;
        case PK_LABEL:    em_label_impl(P.cp, P.str, pr); break;
        case PK_BADGE:    em_badge_impl(P.str, pr); break;
        case PK_TAG:      em_tag_impl(P.str, pr); break;
        case PK_AVATAR:   em_avatar_impl(P.str, pr); break;
        case PK_BANNER:   em_banner_impl(P.cp, P.str, pr); break;
        case PK_PROGRESS: em_progress_impl(P.frac, pr); break;
        case PK_BUTTON:   clicked = em_button_impl(P.str, pr, &hovered); break;
        case PK_ICONBTN:  clicked = em_iconbtn_impl(P.cp, pr, &hovered); break;
        case PK_TOGGLE:   em_toggle_impl(P.str, (bool *)P.bind, pr); break;
        case PK_CHECK:    em_checkbox_impl(P.str, (bool *)P.bind, pr); break;
        case PK_SLIDER:   em_slider_impl((float *)P.bind, pr); break;
        case PK_STEPPER:  em_stepper_impl(P.str, (int *)P.bind, P.lo, P.hi, pr); break;
        case PK_FIELD:    clicked = em_field_impl(P.buf, P.cap, P.str, pr, &hovered); break;  /* clicked == focused */
        case PK_SEGMENTED:em_segmented_impl(P.labels, P.count, (int *)P.bind, pr); break;
        case PK_LISTROW:  clicked = em_listrow_impl(P.cp, P.str, P.str2, pr, &hovered); break;
        case PK_CLOSEBTN: clicked = em_closebtn_impl(&hovered); break;
        case PK_SEARCH:   clicked = em_search_impl(P.buf, P.cap, P.str, &hovered); break;
        case PK_SPINNER:  em_spinner_impl(); break;
        case PK_DROPDOWN: clicked = em_dropdown_impl(P.labels, P.count, (int *)P.bind, &hovered); break;
        default: break;
    }
    P.result = clicked;
    if (id && g_id_n < ID_MAX) { g_ids[g_id_n].id = id; g_ids[g_id_n].clicked = clicked; g_ids[g_id_n].hovered = hovered; g_id_n++; }
}

/* ---- animation clock + eased-scalar animator --------------------------- */
/* Structural-change epoch: bumped whenever a V4 component changes the tree in
 * a way the APP can't observe from its own state (dropdown menu open/close,
 * toast raise/expiry). Apps compare it across frames to force a full repaint
 * (the dirty-rect renderer doesn't erase a removed subtree's pixels). */
static int g_em_epoch;
int em_ui_epoch(void) { return g_em_epoch; }

/* Retained updates: live animations ask for the NEXT frame while active; the
 * app runtime skips all UI work on frames nobody asked for (see em_app.c). */
static int g_frame_req = 1;   /* first frame always builds */
void em_request_frame(void) { g_frame_req = 1; }
int  em_take_frame_request(void) { int r = g_frame_req; g_frame_req = 0; return r; }

static uint64_t (*g_clock)(void);
static uint64_t g_now_ms, g_prev_ms;
static int g_dt_ms;
static int g_ov_now, g_ov_prev, g_ov_dismissed, g_ov_frames;   /* modal-overlay tracking */

void em_set_clock(uint64_t (*fn)(void)) { g_clock = fn; }
uint64_t em_now_ms(void) { return g_clock ? g_clock() : 0; }
int em_dt_ms(void) { return g_dt_ms; }

#define ANIM_MAX 96
static struct { const char *id; float cur, target; int used; } g_anim[ANIM_MAX];

float em_animate(const char *id, float target, float rate) {
    int slot = -1, freei = -1;
    for (int i = 0; i < ANIM_MAX; i++) {
        if (g_anim[i].used) {
            if (g_anim[i].id == id || (g_anim[i].id && id && strcmp(g_anim[i].id, id) == 0)) { slot = i; break; }
        } else if (freei < 0) freei = i;
    }
    if (slot < 0) { slot = freei >= 0 ? freei : 0; g_anim[slot].used = 1; g_anim[slot].id = id; g_anim[slot].cur = target; }
    g_anim[slot].target = target;
    float dt = g_dt_ms / 1000.0f;                 /* exponential approach, frame-rate independent-ish */
    float step = rate * dt;
    if (step > 1.0f) step = 1.0f;
    if (step < 0.0f) step = 0.0f;
    g_anim[slot].cur += (g_anim[slot].target - g_anim[slot].cur) * step;
    float d = g_anim[slot].target - g_anim[slot].cur;
    if (d < 0.0008f && d > -0.0008f) g_anim[slot].cur = g_anim[slot].target;   /* settle */
    else em_request_frame();               /* still easing -> keep frames coming */
    return g_anim[slot].cur;
}

void em_new_frame(void) {
    g_now_ms = em_now_ms();
    g_dt_ms = g_prev_ms ? (int)(g_now_ms - g_prev_ms) : 0;
    if (g_dt_ms < 0) g_dt_ms = 0;
    if (g_dt_ms > 100) g_dt_ms = 100;             /* clamp stalls / first frame */
    g_prev_ms = g_now_ms;
    g_ov_frames = g_ov_now ? g_ov_frames + 1 : 0;   /* consecutive frames shown */
    g_ov_prev = g_ov_now; g_ov_now = 0;             /* overlay-shown tracking for force_full */
    P.active = 0; g_id_n = 0;
}

/* ---- navigation (a page stack of view functions) ----------------------- */
#define NAV_DUR_MS 220.0f
static EmPage g_pages[16];
static int    g_nav_top = -1;
static float  g_nav_t = 1.0f;   /* transition progress: 1 = settled, <1 = fading in */
static int    g_nav_dir = 1;    /* +1 push (slide from right), -1 pop (from left) */
void em_push(EmPage p) { if (p && g_nav_top < 15) { g_pages[++g_nav_top] = p; g_nav_t = 0.0f; g_nav_dir = 1; } }
void em_pop(void)      { if (g_nav_top > 0) { g_nav_top--; g_nav_t = 0.0f; g_nav_dir = -1; } }
int  em_nav_depth(void){ return g_nav_top + 1; }

/* ---- modal overlay (Sheet / Alert) ------------------------------------- *
 * A full-surface dimming scrim with a centred dialog card, declared LAST in the
 * screen so it paints on top. Like a page transition, showing/hiding it is a big
 * structural change the dirty-rect renderer won't fully erase, so the host loop
 * force-repaints while em_overlay_active(). em_overlay_end_ records whether the
 * bare scrim (not the dialog) was clicked -> OverlayDismissed(). */
void em_overlay_(void)      { em_flush(); g_ov_now = 1; ui_overlay_begin(0); }
void em_overlay_end_(void)  {
    em_flush();
    /* Debounce scrim-dismiss for the first few frames after the modal opens: the
     * click that opens it lands on the freshly-created full-screen scrim, and a
     * mid-session tree rebuild can register one more spurious click -- either
     * would open-and-instantly-dismiss the modal. Only honour a real scrim click
     * once the overlay has been stably up for a few frames. */
    g_ov_dismissed = (ui_overlay_end() && g_ov_frames >= 3) ? 1 : 0;
}
int  em_overlay_dismissed(void) { return g_ov_dismissed; }
int  em_overlay_active(void)    { return g_ov_now || g_ov_prev; }
void em_dialog_(EmProps p)  { em_flush(); ui_dialog_begin(0); em_apply_box(p); }
void em_dialog_end_(void)   { em_flush(); ui_dialog_end(); }
/* True while a page transition is fading in. The host loop should force a full
 * surface repaint on these frames: a page swap removes nodes, and the dirty-rect
 * renderer doesn't erase a removed node's vacated pixels, so without a full clear
 * the outgoing page bleeds through the (shorter) incoming one. */
int  em_nav_transitioning(void) { return g_nav_t < 1.0f; }
void em_nav(EmPage root) {
    if (g_nav_top < 0 && root) { g_pages[0] = root; g_nav_top = 0; }
    if (!(g_nav_top >= 0 && g_pages[g_nav_top])) return;

    /* advance the fade-in transition. With no clock set (host render) there's no
     * dt, so snap to settled -- pages are always fully opaque off-device. */
    if (em_now_ms() == 0) {
        g_nav_t = 1.0f;
    } else if (g_nav_t < 1.0f) {
        g_nav_t += (float)em_dt_ms() / NAV_DUR_MS;
        if (g_nav_t > 1.0f) g_nav_t = 1.0f;
        em_request_frame();                /* transition in flight */
    }

    /* Resolve the transition transform. It is applied to the page's OWN root
     * Screen (via g_nav_cur_*), NOT to an extra full-size wrapper box -- the page
     * is already a full-size box, so the second one was redundant. Push/Pop just
     * swaps g_pages[g_nav_top]; the page is called directly as the root here. */
    g_nav_cur_op = 1.0f; g_nav_cur_slide = 0.0f;
    if (g_nav_t < 1.0f) {
        float e = 1.0f - (1.0f - g_nav_t) * (1.0f - g_nav_t);   /* easeOutQuad */
        g_nav_cur_op = 0.12f + 0.88f * e;   /* floor avoids a fully-blank first frame */
        g_nav_cur_slide = (1.0f - e) * 44.0f * (float)g_nav_dir; /* right on push, left on pop */
    }
    em_flush();
    g_pages[g_nav_top]();     /* the page IS the root -- no wrapper rectangle */
    em_flush();
    g_nav_cur_op = 1.0f; g_nav_cur_slide = 0.0f;   /* consumed; don't leak past the page */
}

bool Clicked(const char *id) {
    em_flush();
    for (int i = 0; i < g_id_n; i++)
        if (g_ids[i].id == id || (g_ids[i].id && strcmp(g_ids[i].id, id) == 0)) return g_ids[i].clicked;
    return false;
}
bool Hovered(const char *id) {
    em_flush();
    for (int i = 0; i < g_id_n; i++)
        if (g_ids[i].id == id || (g_ids[i].id && strcmp(g_ids[i].id, id) == 0)) return g_ids[i].hovered;
    return false;
}

/* ---- modifier functions (mutate P; kind-aware where names overlap) ----- */

static const EmV em_v;   /* the singleton vtable, defined below */

static EmV m_font(EmFont f){ P.props.font = f; return em_v; }
static EmV m_title(void){ P.props.font = Title; return em_v; }
static EmV m_heading(void){ P.props.font = Heading; return em_v; }
static EmV m_body(void){ P.props.font = Body; return em_v; }
static EmV m_bold(void){ P.props.font = BodyBold; return em_v; }
static EmV m_caption(void){ P.props.font = Caption; return em_v; }
static EmV m_color(Color c){ P.props.color = c; return em_v; }
static EmV m_secondary(void){ if (P.kind == PK_BUTTON) P.props.style = Secondary; else P.props.color = TH->text_secondary; return em_v; }
static EmV m_tertiary(void){ P.props.color = TH->text_tertiary; return em_v; }
static EmV m_accent(void){ if (P.kind==PK_BADGE||P.kind==PK_TAG||P.kind==PK_BANNER) P.props.tone = Accent; else P.props.color = TH->accent; return em_v; }
static EmV m_primary(void){ P.props.style = Primary; return em_v; }
static EmV m_ghost(void){ P.props.style = Ghost; return em_v; }
static EmV m_destructive(void){ if (P.kind==PK_BUTTON) P.props.style = Destructive; else P.props.tone = Danger; return em_v; }
static EmV m_tone(EmTone t){ P.props.tone = t; return em_v; }
static EmV m_success(void){ P.props.tone = Success; return em_v; }
static EmV m_warning(void){ P.props.tone = Warning; return em_v; }
static EmV m_danger(void){ P.props.tone = Danger; return em_v; }
static EmV m_bg(Color c){ P.props.background = c; return em_v; }
static EmV m_padding(float v){ P.props.padding = v; return em_v; }
static EmV m_px(float v){ P.props.px = v; return em_v; }
static EmV m_py(float v){ P.props.py = v; return em_v; }
static EmV m_frame(float w, float h){ P.props.width = w; P.props.height = h; return em_v; }
static EmV m_width(float w){ P.props.width = w; return em_v; }
static EmV m_height(float h){ P.props.height = h; return em_v; }
static EmV m_grow(void){ P.props.grow = 1; return em_v; }
static EmV m_corner(float r){ P.props.corner = r; return em_v; }
static EmV m_border(float w){ P.props.border = w; return em_v; }
static EmV m_shadow(int n){ P.props.shadow = n; return em_v; }
static EmV m_center(void){ P.props.align = Center; return em_v; }
static EmV m_leading(void){ P.props.align = Leading; return em_v; }
static EmV m_trailing(void){ P.props.align = Trailing; return em_v; }
static EmV m_align(EmAlign a){ P.props.align = a; return em_v; }
static EmV m_id(const char *s){ P.id = s; return em_v; }
static bool m_clicked(void){ em_flush(); return P.result; }
static bool m_focused(void){ em_flush(); return P.result; }

static const EmV em_v = {
    .title=m_title, .heading=m_heading, .body=m_body, .bold=m_bold, .caption=m_caption, .font=m_font,
    .color=m_color, .secondary=m_secondary, .tertiary=m_tertiary, .accent=m_accent,
    .primary=m_primary, .ghost=m_ghost, .destructive=m_destructive,
    .tone=m_tone, .success=m_success, .warning=m_warning, .danger=m_danger,
    .bg=m_bg, .padding=m_padding, .px=m_px, .py=m_py, .frame=m_frame, .width=m_width, .height=m_height, .grow=m_grow,
    .corner=m_corner, .border=m_border, .shadow=m_shadow,
    .center=m_center, .leading=m_leading, .trailing=m_trailing, .align=m_align,
    .id=m_id, .clicked=m_clicked, .focused=m_focused,
};

/* ---- creators (stage a pending element, return the chain) -------------- */

static EmV stage(PKind k) { em_flush(); memset(&P, 0, sizeof P); P.active = 1; P.kind = k; return em_v; }

EmV em_text(const char *s){ EmV v = stage(PK_TEXT); P.str = s; return v; }
EmV em_icon(int cp){ EmV v = stage(PK_ICON); P.cp = cp; return v; }
EmV em_label(int cp, const char *s){ EmV v = stage(PK_LABEL); P.cp = cp; P.str = s; return v; }
EmV em_badge(const char *s){ EmV v = stage(PK_BADGE); P.str = s; return v; }
EmV em_tag(const char *s){ EmV v = stage(PK_TAG); P.str = s; return v; }
EmV em_avatar(const char *s){ EmV v = stage(PK_AVATAR); P.str = s; return v; }
EmV em_banner(int cp, const char *s){ EmV v = stage(PK_BANNER); P.cp = cp; P.str = s; return v; }
EmV em_progress(float f){ EmV v = stage(PK_PROGRESS); P.frac = f; return v; }
EmV em_button(const char *s){ EmV v = stage(PK_BUTTON); P.str = s; return v; }
EmV em_icon_button(int cp){ EmV v = stage(PK_ICONBTN); P.cp = cp; return v; }
EmV em_toggle(const char *l, bool *b){ EmV v = stage(PK_TOGGLE); P.str = l; P.bind = b; return v; }
EmV em_checkbox(const char *l, bool *b){ EmV v = stage(PK_CHECK); P.str = l; P.bind = b; return v; }
EmV em_slider(float *b){ EmV v = stage(PK_SLIDER); P.bind = b; return v; }
EmV em_stepper(const char *l, int *b, int lo, int hi){ EmV v = stage(PK_STEPPER); P.str = l; P.bind = b; P.lo = lo; P.hi = hi; return v; }
EmV em_text_field(char *buf, size_t cap, const char *ph){ EmV v = stage(PK_FIELD); P.buf = buf; P.cap = cap; P.str = ph; return v; }
EmV em_segmented(const char *const *labels, int count, int *b){ EmV v = stage(PK_SEGMENTED); P.labels = labels; P.count = count; P.bind = b; return v; }
EmV em_listrow(int icon, const char *title, const char *value){ EmV v = stage(PK_LISTROW); P.cp = icon; P.str = title; P.str2 = value; return v; }
EmV em_close_button(void){ EmV v = stage(PK_CLOSEBTN); P.id = "__em_win_close"; return v; }
EmV em_search_field(char *buf, size_t cap, const char *ph){ EmV v = stage(PK_SEARCH); P.buf = buf; P.cap = cap; P.str = ph; return v; }
EmV em_spinner(void){ return stage(PK_SPINNER); }
EmV em_dropdown(const char *const *labels, int count, int *sel){ EmV v = stage(PK_DROPDOWN); P.labels = labels; P.count = count; P.bind = sel; return v; }
void em_spacer_(void){ em_flush(); ui_spacer(); }
void em_divider_(void){ em_flush(); ui_divider(); }

/* ======================================================================= */
/* emitters                                                                */
/* ======================================================================= */

static int props_wrap(EmProps p) {
    return (p.padding > 0 || p.px > 0 || p.py > 0 || p.pt > 0 || p.pr > 0 || p.pb > 0 || p.pl > 0 ||
            p.background.a > 0 || p.corner > 0 || p.border > 0 || p.width > 0 || p.grow);
}

static void em_text_impl(const char *s, EmProps p) {
    uint32_t fh; float sz; em_resolve_font(p.font, &fh, &sz);
    Color col = p.color.a > 0 ? p.color : TH->text;
    if (props_wrap(p)) {
        ui_box_begin(0);
        em_apply_box(p);
        ui_set_align(ALIGN_CENTER);
        ui_set_font(fh); ui_set_text_size(sz); ui_set_text_color(col);
        ui_text("%s", s);
        ui_box_end();
    } else {
        ui_set_font(fh); ui_set_text_size(sz); ui_set_text_color(col);
        ui_text("%s", s);
    }
}
static void em_icon_impl(int cp, EmProps p) {
    char g[5]; utf8_enc(cp, g);
    if (!p.font) p.font = Body;
    em_text_impl(g, p);
}
static void em_label_impl(int cp, const char *s, EmProps p) {
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(6);
    em_apply_box(p);
    EmProps ip = { .font = p.font ? p.font : Body, .color = p.color };
    em_icon_impl(cp, ip);
    EmProps tp = { .font = p.font ? p.font : Body, .color = p.color };
    em_text_impl(s, tp);
    ui_end_stack();
}

static enum ui_badge_tone map_tone(EmTone tn) {
    switch (tn) { case Success: return BADGE_SUCCESS; case Warning: return BADGE_WARNING;
                  case Danger: return BADGE_DANGER; case Neutral: return BADGE_NEUTRAL; default: return BADGE_ACCENT; }
}
static void em_badge_impl(const char *s, EmProps p) { ui_badge(s, map_tone(p.tone)); }

static void em_tag_impl(const char *s, EmProps p) {
    const struct ui_theme *t = TH;
    Color fg = t->text_secondary;
    switch (p.tone) { case Accent: fg = t->accent; break; case Success: fg = t->success; break;
                      case Warning: fg = t->warning; break; case Danger: fg = t->danger; break; default: break; }
    ui_begin_hstack(0);
    ui_set_paint(solid(t->surface_alt));
    ui_set_border(1.0f, t->border);
    ui_set_corner_radius(t->radius_pill);
    ui_set_padding(t->sp1, t->sp2 + 2, t->sp1, t->sp2 + 2);
    ui_set_align(ALIGN_CENTER);
    ui_set_font(t->font_bold); ui_set_text_size(t->text_caption); ui_set_text_color(fg);
    ui_text("%s", s);
    ui_end_stack();
}
static void em_avatar_impl(const char *s, EmProps p) { (void)p; ui_avatar(s); }

static void em_banner_impl(int cp, const char *msg, EmProps p) {
    const struct ui_theme *t = TH;
    Color accent = t->accent;
    switch (p.tone) { case Success: accent = t->success; break; case Warning: accent = t->warning; break;
                      case Danger: accent = t->danger; break; default: accent = t->accent; break; }
    ui_begin_hstack(0);
    ui_set_paint(solid(tint(accent, 0.14f)));
    ui_set_corner_radius(t->radius_md);
    ui_set_border(1.0f, tint(accent, 0.35f));
    ui_set_padding(t->sp3, t->sp4, t->sp3, t->sp4);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp3);
    { EmProps ip = { .font = Body, .color = accent }; em_icon_impl(cp, ip); }
    { EmProps tp = { .font = Body, .color = t->text }; em_text_impl(msg, tp); }
    ui_spacer();
    ui_end_stack();
}
static void em_progress_impl(float frac, EmProps p) { (void)p; ui_progress(frac); }

static bool em_button_impl(const char *s, EmProps p, bool *out_hov) {
    const struct ui_theme *t = TH;
    ui_box_begin(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    if (out_hov) *out_hov = hov;
    Color fill = t->accent, txt = t->on_accent, bcol = t->border_strong;
    int has_fill = 1, has_border = 0;
    switch (p.style) {
        case Secondary:   fill = t->surface; txt = t->text; has_border = 1; break;
        case Ghost:       has_fill = 0; txt = t->accent; break;
        case Destructive: fill = t->danger; txt = t->on_accent; break;
        default: break;
    }
    if (p.background.a > 0) { fill = p.background; has_fill = 1; }
    if (p.color.a > 0) txt = p.color;
    if (has_fill) { Color f = pressed ? shade(fill, 0.86f) : hov ? shade(fill, 1.10f) : fill; ui_set_paint(solid(f)); }
    else if (hov) ui_set_paint(solid(shade(t->accent_soft, pressed ? 0.9f : 1.0f)));
    ui_set_corner_radius(p.corner > 0 ? p.corner : t->radius_md);
    if (has_border || p.border > 0) ui_set_border(p.border > 0 ? p.border : 1.0f, hov ? t->accent : bcol);
    ui_set_padding(t->sp2 + 1, t->sp4, t->sp2 + 1, t->sp4);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    if (p.grow || p.width > 0) { struct layout_size w = p.width > 0 ? sz_fixed(p.width) : sz_grow(); ui_set_size(w, sz_intrinsic()); }
    uint32_t fh; float sz; em_resolve_font(p.font ? p.font : BodyBold, &fh, &sz);
    ui_set_font(fh); ui_set_text_size(sz); ui_set_text_color(txt);
    ui_text("%s", s);
    ui_box_end();
    return ui_consume_click(self);
}
static bool em_iconbtn_impl(int cp, EmProps p, bool *out_hov) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    if (out_hov) *out_hov = hov;
    Color bg = p.background.a > 0 ? p.background : t->surface_alt;
    ui_set_paint(solid(pressed ? shade(bg, 0.9f) : hov ? shade(bg, 1.12f) : bg));
    ui_set_corner_radius(p.corner > 0 ? p.corner : t->radius_md);
    ui_set_padding(t->sp2, t->sp2, t->sp2, t->sp2);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    EmProps ip = { .font = p.font ? p.font : Body, .color = p.color.a > 0 ? p.color : t->text_secondary };
    em_icon_impl(cp, ip);
    ui_end_stack();
    return ui_consume_click(self);
}
static void em_toggle_impl(const char *label, bool *bind, EmProps p) {
    ui_begin_hstack(0); ui_set_align(ALIGN_CENTER); ui_set_spacing(TH->sp3);
    em_apply_box(p);
    if (label && label[0]) { EmProps tp = { .font = Body }; em_text_impl(label, tp); }
    ui_spacer();
    if (ui_toggle(bind ? *bind : false) && bind) *bind = !*bind;
    ui_end_stack();
}
static void em_checkbox_impl(const char *label, bool *bind, EmProps p) {
    ui_begin_hstack(0); ui_set_align(ALIGN_CENTER); ui_set_spacing(TH->sp3);
    em_apply_box(p);
    if (ui_checkbox(bind ? *bind : false) && bind) *bind = !*bind;
    if (label && label[0]) { EmProps tp = { .font = Body }; em_text_impl(label, tp); }
    ui_spacer();
    ui_end_stack();
}
static void em_slider_impl(float *bind, EmProps p) {
    (void)p; float v = ui_slider(bind ? *bind : 0.0f); if (bind) *bind = v;
}
static void em_stepper_impl(const char *label, int *bind, int lo, int hi, EmProps p) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0); ui_set_align(ALIGN_CENTER); ui_set_spacing(t->sp3);
    em_apply_box(p);
    if (label && label[0]) { EmProps tp = { .font = Body }; em_text_impl(label, tp); }
    ui_spacer();
    ui_begin_hstack(0);
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_md);
    ui_set_padding(2, t->sp2, 2, t->sp2);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    if (em_iconbtn_impl(IconMinus, (EmProps){0}, 0) && bind && *bind > lo) (*bind)--;
    { char b[16]; snprintf(b, sizeof b, "%d", bind ? *bind : 0); EmProps vp = { .font = BodyBold }; em_text_impl(b, vp); }
    if (em_iconbtn_impl(IconPlus, (EmProps){0}, 0) && bind && *bind < hi) (*bind)++;
    ui_end_stack();
    ui_end_stack();
}
static bool em_field_impl(char *buf, size_t cap, const char *ph, EmProps p, bool *out_hov) {
    (void)p; if (out_hov) *out_hov = false;
    return ui_text_field(buf, cap, ph);
}
static void em_segmented_impl(const char *const *labels, int count, int *bind, EmProps p) {
    (void)p; int cur = bind ? *bind : 0; int nv = ui_segmented(labels, count, cur); if (bind) *bind = nv;
}

/* ---- richer components ------------------------------------------------- */

/* Bar chart: values scaled to the max, bottom-aligned, last bar emphasised. */
void em_chart(const float *vals, int n, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    float mx = 0.00001f;
    for (int i = 0; i < n; i++) if (vals[i] > mx) mx = vals[i];
    float H = p.height > 0 ? p.height : 84.0f;
    Color col = p.color.a > 0 ? p.color : t->accent;
    ui_begin_hstack(0);
    ui_set_size(sz_grow(), sz_fixed(H));
    ui_set_align(ALIGN_END);                          /* bars grow up from the baseline */
    ui_set_spacing(p.spacing > 0 ? p.spacing : 5);
    for (int i = 0; i < n; i++) {
        float bh = 4.0f + (vals[i] / mx) * (H - 4.0f);
        ui_box_begin((uint64_t)(i + 1));
        ui_set_paint(solid(i == n - 1 ? col : tint(col, 0.45f)));   /* emphasise the latest */
        ui_set_corner_radius(3);
        ui_set_size(sz_grow(), sz_fixed(bh));
        ui_box_end();
    }
    ui_end_stack();
}

/* ---- line / area chart ------------------------------------------------- *
 * A software line rasteriser (the "line primitive") draws a polyline -- and,
 * for an area chart, the fill beneath it -- into a persistent premultiplied
 * BGRA bitmap, shown via an IMAGE node (draw_image scales it to the row width).
 * One shared buffer -> one line chart on screen at a time; data is treated as
 * fixed (scene_set_image guards on the pointer, so a redraw of the same buffer
 * doesn't re-dirty -- fine for static series). */
#define LC_W 260
#define LC_H 100
static uint32_t g_lc_buf[LC_W * LC_H];

static uint32_t argb_premul(Color c, float a) {
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    uint32_t A = (uint32_t)(a * 255.0f + 0.5f);
    uint32_t R = (uint32_t)(c.r * a * 255.0f + 0.5f);
    uint32_t G = (uint32_t)(c.g * a * 255.0f + 0.5f);
    uint32_t B = (uint32_t)(c.b * a * 255.0f + 0.5f);
    return (A << 24) | (R << 16) | (G << 8) | B;
}
static inline void lc_plot(int x, int y, uint32_t c) {
    if (x >= 0 && x < LC_W && y >= 0 && y < LC_H) g_lc_buf[y * LC_W + x] = c;
}

/* filled=1 -> area chart (fill under the line); filled=0 -> plain line. */
void em_linechart(const float *vals, int n, int filled, EmProps p) {
    em_flush();
    if (n < 2) return;
    const struct ui_theme *t = TH;
    Color ac = p.color.a > 0 ? p.color : t->accent;
    uint32_t line = argb_premul(ac, 1.0f);
    uint32_t dot  = argb_premul(ac, 1.0f);
    uint32_t fill = argb_premul(ac, 0.26f);

    for (int i = 0; i < LC_W * LC_H; i++) g_lc_buf[i] = 0;   /* transparent */

    float mx = -1e30f, mn = 1e30f;
    for (int i = 0; i < n; i++) { if (vals[i] > mx) mx = vals[i]; if (vals[i] < mn) mn = vals[i]; }
    if (mx <= mn) mx = mn + 1.0f;
    const int pad = 8, base = LC_H - pad;
    #define LC_Y(v) ((float)base - ((v) - mn) / (mx - mn) * (float)(LC_H - 2 * pad))

    for (int i = 0; i < n - 1; i++) {
        float fx0 = (float)i / (n - 1) * (LC_W - 1), fx1 = (float)(i + 1) / (n - 1) * (LC_W - 1);
        float fy0 = LC_Y(vals[i]), fy1 = LC_Y(vals[i + 1]);
        int x0 = (int)(fx0 + 0.5f), x1 = (int)(fx1 + 0.5f);
        for (int x = x0; x <= x1 && x < LC_W; x++) {
            float tt = x1 > x0 ? (float)(x - x0) / (float)(x1 - x0) : 0.0f;
            int y = (int)(fy0 + (fy1 - fy0) * tt + 0.5f);
            if (filled) for (int yy = y; yy < base; yy++) lc_plot(x, yy, fill);
            lc_plot(x, y, line); lc_plot(x, y - 1, line);   /* 2px line */
        }
    }
    /* sample dots */
    for (int i = 0; i < n; i++) {
        int cx = (int)((float)i / (n - 1) * (LC_W - 1) + 0.5f), cy = (int)(LC_Y(vals[i]) + 0.5f);
        for (int oy = -2; oy <= 2; oy++) for (int ox = -2; ox <= 2; ox++)
            if (ox*ox + oy*oy <= 4) lc_plot(cx + ox, cy + oy, dot);
    }
    #undef LC_Y

    float H = p.height > 0 ? p.height : 100.0f;
    ui_image((uint64_t)(uintptr_t)&g_lc_buf, g_lc_buf, LC_W, LC_H, H);
}

/* Grouped inset list container. */
void em_list_(EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    ui_set_paint(solid(p.background.a > 0 ? p.background : t->surface_alt));
    ui_set_corner_radius(p.corner > 0 ? p.corner : t->radius_md);
    ui_set_border(1.0f, t->border);
    ui_set_clip_children(true);
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(0);
    em_apply_box(p);
}
void em_list_end_(void) { em_flush(); ui_end_stack(); }

/* A tappable list row: [icon] title ........ value  ›   */
static bool em_listrow_impl(int cp, const char *title, const char *value, EmProps p, bool *out_hov) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    if (out_hov) *out_hov = hov;
    ui_set_paint(solid(pressed ? shade(t->surface_alt, 0.94f) : hov ? shade(t->surface, 1.06f) : t->surface));
    ui_set_padding(t->sp3, t->sp4, t->sp3, t->sp4);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp3);
    if (cp) { EmProps ip = { .font = Body, .color = p.color.a > 0 ? p.color : t->accent }; em_icon_impl(cp, ip); }
    { EmProps tp = { .font = Body }; em_text_impl(title, tp); }
    ui_spacer();
    if (value && value[0]) { EmProps vp = { .font = Body, .color = t->text_secondary }; em_text_impl(value, vp); }
    { EmProps cp2 = { .font = Body, .color = t->text_tertiary }; em_icon_impl(IconChevronR, cp2); }
    ui_end_stack();
    return ui_consume_click(self);
}

/* ======================================================================= */
/* EmUI V4 implementation                                                  */
/* ======================================================================= */

/* ---- app-owned window chrome ------------------------------------------ */
/* The toolkit stays syscall-free: the app registers HOW to move its window
 * (em_window_set_mover, mirroring em_set_clock) and its id + current screen
 * position (em_window_bind). WindowBar's drag then just calls the mover. */
static void   (*g_win_mover)(int win, int32_t x, int32_t y);
static int      g_win_bound;
static int      g_win_id;
static int32_t  g_win_x, g_win_y;      /* window's current screen top-left */
static int      g_win_dragging;
static float    g_win_grab_x, g_win_grab_y;   /* pointer-at-grab, content-local */

void em_window_set_mover(void (*mover)(int win, int32_t x, int32_t y)) { g_win_mover = mover; }
void em_window_bind(int win, int32_t x, int32_t y) {
    g_win_id = win; g_win_x = x; g_win_y = y; g_win_bound = 1;
}

/* resizable-window plumbing (V5): the grip accumulates a drag delta and, on
 * RELEASE, parks it here for the runtime to apply (live re-backing every frame
 * would thrash the page allocator; commit-on-release keeps it one realloc). */
static int g_win_resizable;
static int g_win_glass;
static int g_rz_active, g_rz_pend;
static float g_rz_grab_x, g_rz_grab_y, g_rz_dx, g_rz_dy;
void em_window_set_resizable(int on) { g_win_resizable = on; }
void em_window_set_glass(int on) { g_win_glass = on; }
int em_window_take_resize(int *dw, int *dh) {
    if (!g_rz_pend) return 0;
    g_rz_pend = 0;
    if (dw) *dw = (int)g_rz_dx;
    if (dh) *dh = (int)g_rz_dy;
    return 1;
}

/* ---- drag-to-dismiss close (EmbLink's own close gesture) ----------------- *
 * Instead of a fixed X button, the window has a GRIP you pull: as you drag it
 * the window fades + slides toward the pull (a "peel away" preview); release
 * past the threshold and it closes, release early and it springs back. Same
 * commit-on-release shape as the resize grip. The runtime reads the commit via
 * em_window_take_close(); the visual (fade/slide) is applied by em_window_. */
#define CLOSE_PULL 95.0f           /* px of drag that commits the close */
static int   g_cl_active, g_cl_pend;
static float g_cl_grab_x, g_cl_grab_y, g_cl_dx, g_cl_dy, g_cl_progress;

int em_window_take_close(void) { if (!g_cl_pend) return 0; g_cl_pend = 0; return 1; }

bool em_close_grip(void) {
    const struct ui_theme *t = TH;
    float pr = g_cl_progress;
    ui_begin_hstack(0xC105E);   /* stable key so the drag-owner identity survives rebuilds */
    struct instance_handle self = ui_open(); (void)self;
    bool active = ui_is_active();
    /* a pull HANDLE (wider than a button, so it reads as draggable), tinting from
     * the surface toward danger-red as the pull progresses. */
    Color bg = { t->surface.r + (t->danger.r - t->surface.r) * pr,
                 t->surface.g + (t->danger.g - t->surface.g) * pr,
                 t->surface.b + (t->danger.b - t->surface.b) * pr, 1.0f };
    ui_set_paint(solid(bg));
    ui_set_corner_radius(t->radius_pill);
    ui_set_border(1.0f, (active || pr > 0.05f) ? t->danger : t->border);
    ui_set_size(sz_fixed(48), sz_fixed(28));
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    { EmProps ip = { .font = BodyBold, .color = pr > 0.5f ? t->on_accent : t->text_secondary };
      em_icon_impl(IconClose, ip); }
    ui_end_stack();

    if (active) {
        float px, py; ui_pointer_pos(&px, &py);
        if (!g_cl_active) { g_cl_active = 1; g_cl_grab_x = px; g_cl_grab_y = py; }
        g_cl_dx = px - g_cl_grab_x; g_cl_dy = py - g_cl_grab_y;
        float dist = (g_cl_dx < 0 ? -g_cl_dx : g_cl_dx) + (g_cl_dy < 0 ? -g_cl_dy : g_cl_dy);
        g_cl_progress = dist / CLOSE_PULL;
        if (g_cl_progress >= 1.0f) { g_cl_progress = 1.0f; g_cl_pend = 1; }  /* pulled far enough -> close */
        em_request_frame();
    } else if (g_cl_active) {
        g_cl_active = 0;                               /* released before the threshold */
    }
    /* spring back when released below the threshold */
    if (!g_cl_active && !g_cl_pend && g_cl_progress > 0.001f) {
        g_cl_progress *= 0.72f; g_cl_dx *= 0.72f; g_cl_dy *= 0.72f;
        if (g_cl_progress < 0.02f) { g_cl_progress = 0; g_cl_dx = g_cl_dy = 0; }
        em_request_frame();
    }
    return g_cl_pend != 0;
}

/* True while a close pull is in progress (or springing back) -- the app runtime
 * force-repaints so the fade/slide don't ghost under the dirty-rect present. */
int em_window_pulling(void) { return g_cl_active || g_cl_progress > 0.001f; }

/* Window: the full-bleed top-level surface of a chromeless app window. Fills
 * the whole pixel buffer with the theme background (rectangular -- the OS
 * window has no per-pixel alpha), lays children in a stretched column. */
void em_window_(const char *title, EmProps p) {
    (void)title;
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    /* the app renders opaque (the compositor makes a glass window translucent
     * and frosts the desktop behind it); a glass window just gets a faint accent
     * cast in its bg so the frost reads as EmbLink's, not a neutral gray. */
    Color bg = p.background.a > 0 ? p.background : t->bg;
    if (g_win_glass) { Color a = t->accent;
        bg = (Color){ bg.r * 0.92f + a.r * 0.08f, bg.g * 0.92f + a.g * 0.08f,
                      bg.b * 0.92f + a.b * 0.08f, 1.0f }; }
    ui_set_paint(solid(bg));
    ui_set_size(sz_grow(), sz_grow());
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(0);
    em_apply_box(p);
    /* drag-to-dismiss preview: fade + slide the whole window toward the pull as
     * the close grip is dragged (springs back if released early). */
    if (g_cl_progress > 0.001f) {
        ui_set_opacity(1.0f - g_cl_progress * 0.88f);
        ui_set_offset(g_cl_dx * 0.30f, g_cl_dy * 0.30f);
    }
}
void em_window_end_(void) {
    em_flush();
    if (g_win_resizable) {
        /* corner grip: a zero-height anchor at the window's very bottom, its
         * glyph raised into the corner with a scene offset (same trick as the
         * Toast). While held it tracks the pointer; on release it parks the
         * delta for the runtime. */
        const struct ui_theme *t = TH;
        ui_begin_hstack(0);
        ui_set_size(sz_grow(), (struct layout_size){ SIZE_FIXED, 0, 0, 0, 0 });
        ui_set_justify(JUSTIFY_END);
        {
            ui_begin_hstack(1);
            struct instance_handle grip = ui_open(); (void)grip;
            bool active = ui_is_active();
            ui_set_size(sz_fixed(20), sz_fixed(20));
            ui_set_align(ALIGN_CENTER);
            ui_set_justify(JUSTIFY_CENTER);
            ui_set_offset(0, -20.0f);
            { char g[5]; utf8_enc(0x25E2, g);   /* black lower-right triangle */
              ui_set_font(t->font_regular); ui_set_text_size(t->text_caption);
              ui_set_text_color(active ? t->accent : t->text_tertiary);
              ui_text("%s", g); }
            ui_end_stack();
            if (active) {
                float px, py; ui_pointer_pos(&px, &py);
                if (!g_rz_active) { g_rz_active = 1; g_rz_grab_x = px; g_rz_grab_y = py; g_rz_dx = 0; g_rz_dy = 0; }
                else { g_rz_dx = px - g_rz_grab_x; g_rz_dy = py - g_rz_grab_y; }
                em_request_frame();               /* keep tracking while held */
            } else if (g_rz_active) {
                g_rz_active = 0;
                if (g_rz_dx > 4.0f || g_rz_dx < -4.0f || g_rz_dy > 4.0f || g_rz_dy < -4.0f)
                    g_rz_pend = 1;                /* commit on release */
            }
        }
        ui_end_stack();
    }
    ui_end_stack();
}

/* The drag zone's per-frame move logic. Pointer is content-local; because the
 * app tracks its own origin, moving the window by (ptr_now - grab) converges in
 * one step (after the move the kernel re-references the pointer to the new
 * origin, so the grab point stays put). See the plan's drag derivation. */
static void em_window_drag_(void) {
    if (!g_win_bound) return;
    if (ui_is_active()) {          /* pressed on the drag zone, button still held */
        float px, py; ui_pointer_pos(&px, &py);
        if (!g_win_dragging) { g_win_dragging = 1; g_win_grab_x = px; g_win_grab_y = py; }
        else {
            int nx = g_win_x + (int)(px - g_win_grab_x);
            int ny = g_win_y + (int)(py - g_win_grab_y);
            if ((nx != g_win_x || ny != g_win_y) && g_win_mover) {
                g_win_mover(g_win_id, nx, ny);
                g_win_x = nx; g_win_y = ny;
            }
        }
    } else {
        g_win_dragging = 0;
    }
}

/* WindowBar: a draggable strip. Its interior is a growing "drag zone" that
 * carries the title and absorbs the drag; the scope's children append AFTER it
 * as siblings (right side), so pressing a control -- e.g. CloseButton -- never
 * starts a drag. Fully restyleable through EmProps/theme. */
void em_windowbar_(const char *title, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);                              /* the bar */
    ui_set_paint(solid(p.background.a > 0 ? p.background : t->surface_alt));
    ui_set_padding(t->sp2, t->sp3, t->sp2, t->sp3);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_set_border(0, t->border);                     /* a hairline under the bar reads as chrome */
    em_apply_box(p);

    ui_begin_hstack(0);                              /* drag zone (grows, grabbable) */
    ui_open();
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    em_window_drag_();
    { EmProps dp = { .font = Body, .color = t->text_tertiary }; em_icon_impl(IconDot, dp); }
    { EmProps tp = { .font = BodyBold, .color = t->text }; em_text_impl(title && title[0] ? title : " ", tp); }
    ui_spacer();
    ui_end_stack();                                  /* close drag zone; controls follow as siblings */
}
void em_windowbar_end_(void) { em_flush(); ui_end_stack(); }

/* A modern single circular close control (no traffic-light trio). Hover tints
 * it danger-red. Chainable: `CloseButton().clicked()`. */
static bool em_closebtn_impl(bool *out_hov) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    if (out_hov) *out_hov = hov;
    Color bg = hov ? (pressed ? shade(t->danger, 0.86f) : t->danger) : t->surface;
    Color fg = hov ? t->on_accent : t->text_secondary;
    ui_set_paint(solid(bg));
    ui_set_corner_radius(t->radius_pill);
    ui_set_border(hov ? 0.0f : 1.0f, t->border);
    ui_set_size(sz_fixed(26), sz_fixed(26));
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    { EmProps ip = { .font = BodyBold, .color = fg }; em_icon_impl(IconClose, ip); }
    ui_end_stack();
    return ui_consume_click(self);
}
int em_window_closed(void) { return Clicked("__em_win_close"); }

/* ---- Spinner: phase-animated dots (indeterminate activity) ------------- */
static void em_spinner_impl(void) {
    const struct ui_theme *t = TH;
    em_request_frame();                    /* indeterminate: animates every frame */
    const int N = 8;
    float now = (float)em_now_ms();
    float phase = now > 0 ? (now / 90.0f) : 0.0f;   /* advance ~11 steps/sec */
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp1 + 1);
    for (int i = 0; i < N; i++) {
        /* a lit dot travels around the row */
        float d = phase - (float)i;
        int k = ((int)d) % N; if (k < 0) k += N;
        float a = (k == 0) ? 1.0f : (k == 1 || k == N - 1) ? 0.55f : 0.20f;
        ui_box_begin((uint64_t)(i + 1));
        ui_set_paint(solid(tint(t->accent, a)));
        ui_set_corner_radius(t->radius_pill);
        ui_set_size(sz_fixed(7), sz_fixed(7));
        ui_box_end();
    }
    ui_end_stack();
}

/* ---- Gauge: a rasterised ring (0..1), like LineChart --------------------*/
#define GA_SZ 120
static uint32_t g_ga_buf[GA_SZ * GA_SZ];

/* fast atan2 (~0.01 rad) -> avoids a libm dependency in the toolkit. */
static float em_atan2(float y, float x) {
    const float PI = 3.14159265f, HALF = 1.57079633f;
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, r;
    if (ax >= ay) { float z = ay / (ax + 1e-9f); r = z * (0.9724f - 0.1919f * z * z); }
    else          { float z = ax / (ay + 1e-9f); r = HALF - z * (0.9724f - 0.1919f * z * z); }
    if (x < 0) r = PI - r;
    if (y < 0) r = -r;
    return r;
}

void em_gauge(float frac, const char *center, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    Color ac = p.color.a > 0 ? p.color : t->accent;
    uint32_t on  = argb_premul(ac, 1.0f);
    uint32_t off = argb_premul(t->border, 1.0f);
    /* Re-rasterise ONLY when the ring actually changes: per-pixel float math
     * every frame is poison under TCG (it alone pushed v4demo's frame time to
     * ~0.5s). Keyed on the quantised fraction + both colours. */
    static uint32_t ga_key;
    uint32_t key = ((uint32_t)(frac * 1024.0f) << 8) ^ on ^ (off * 2654435761u);
    if (key != ga_key || ga_key == 0) {
        ga_key = key;
        const float TWO_PI = 6.2831853f;
        float cx = GA_SZ / 2.0f, cy = GA_SZ / 2.0f;
        float r_out = GA_SZ / 2.0f - 2.0f, r_in = r_out - 12.0f;
        for (int y = 0; y < GA_SZ; y++) {
            for (int x = 0; x < GA_SZ; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                float dist = dx * dx + dy * dy;
                uint32_t px = 0;                               /* transparent */
                if (dist <= r_out * r_out && dist >= r_in * r_in) {
                    float turn = em_atan2(dx, -dy) / TWO_PI;   /* 0 at top, CW */
                    if (turn < 0) turn += 1.0f;
                    px = (turn <= frac) ? on : off;
                }
                g_ga_buf[y * GA_SZ + x] = px;
            }
        }
    }
    float H = p.height > 0 ? p.height : (float)GA_SZ;
    /* ring + a centred readout beneath it (a column keeps layout predictable in
     * the pure-translation scene -- no fragile absolute overlay). */
    ui_begin_vstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp1);
    ui_image((uint64_t)(uintptr_t)&g_ga_buf, g_ga_buf, GA_SZ, GA_SZ, H);
    if (center && center[0]) {
        ui_set_font(t->font_bold); ui_set_text_size(t->text_title); ui_set_text_color(t->text);
        ui_text("%s", center);
    }
    ui_end_stack();
}

/* ---- StatCard: label / big value / signed delta / mini sparkline -------- */
void em_stat_card(const char *label, const char *value, const char *delta,
                  const float *vals, int n) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_box_begin(0);
    ui_set_paint(solid(t->surface));
    ui_set_corner_radius(t->radius_lg);
    ui_set_border(1.0f, t->border);
    ui_set_padding(t->sp4, t->sp4, t->sp4, t->sp4);
    ui_set_axis(AXIS_COLUMN);
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp1);
    ui_set_size(sz_grow(), sz_intrinsic());

    { EmProps lp = { .font = Caption, .color = t->text_secondary }; em_text_impl(label, lp); }
    { EmProps vp = { .font = Title, .color = t->text }; em_text_impl(value, vp); }
    if (delta && delta[0]) {
        int neg = (delta[0] == '-');
        Color dc = neg ? t->danger : t->success;
        EmProps dp = { .font = Caption, .color = dc };
        em_text_impl(delta, dp);
    }
    if (vals && n >= 2) {
        EmProps cp = { .height = 34, .color = t->accent };
        em_linechart(vals, n, 1, cp);
    }
    ui_box_end();
}

/* ---- EmptyState: centred icon + title + subtitle ----------------------- */
void em_empty_state(int icon, const char *title, const char *subtitle) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    ui_set_spacing(t->sp2);
    ui_set_padding(t->sp6, t->sp5, t->sp6, t->sp5);
    ui_set_size(sz_grow(), sz_intrinsic());
    { char g[5]; utf8_enc(icon, g);
      ui_set_font(t->font_regular); ui_set_text_size(t->text_heading * 1.6f);
      ui_set_text_color(t->text_tertiary); ui_text("%s", g); }
    { EmProps tp = { .font = Heading, .color = t->text }; em_text_impl(title, tp); }
    if (subtitle && subtitle[0]) {
        EmProps sp = { .font = Body, .color = t->text_secondary };
        em_text_impl(subtitle, sp);
    }
    ui_end_stack();
}

/* ---- DividerLabel: line -- LABEL -- line -------------------------------- */
void em_divider_label(const char *label) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp3);
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_box_begin(1); ui_set_paint(solid(t->border)); ui_set_size(sz_grow(), sz_fixed(1)); ui_box_end();
    { ui_set_font(t->font_bold); ui_set_text_size(t->text_caption); ui_set_text_color(t->text_tertiary);
      ui_text("%s", label); }
    ui_box_begin(2); ui_set_paint(solid(t->border)); ui_set_size(sz_grow(), sz_fixed(1)); ui_box_end();
    ui_end_stack();
}

/* ---- Toast: transient message; ToastHost() renders the active one ------ */
static struct { const char *msg; EmTone tone; uint64_t raised; int active; } g_toast;
#define TOAST_MS 2500

void em_toast(const char *msg, EmTone tone) {
    g_toast.msg = msg; g_toast.tone = tone; g_toast.raised = em_now_ms(); g_toast.active = 1;
    g_em_epoch++;
}
void em_toast_host(void) {
    if (!g_toast.active) return;
    em_request_frame();                    /* fading/expiring: keep frames coming */
    const struct ui_theme *t = TH;
    uint64_t now = em_now_ms();
    /* with a real clock, auto-expire; on host (clock 0) stay visible for render */
    float age = (now && g_toast.raised) ? (float)(now - g_toast.raised) : 0.0f;
    if (now && age > TOAST_MS) { g_toast.active = 0; g_em_epoch++; return; }
    /* fade+rise in over the first 180ms and out over the last 300ms */
    float op = 1.0f;
    if (now) {
        if (age < 180.0f) op = age / 180.0f;
        else if (age > TOAST_MS - 300.0f) op = (TOAST_MS - age) / 300.0f;
    }
    if (op < 0) op = 0;
    if (op > 1) op = 1;
    Color accent = t->accent;
    int cp = IconInfo;
    switch (g_toast.tone) {
        case Success: accent = t->success; cp = IconCheck; break;
        case Warning: accent = t->warning; cp = IconWarn;  break;
        case Danger:  accent = t->danger;  cp = IconClose; break;
        default: break;
    }
    /* A ZERO-HEIGHT in-flow anchor at the very bottom of the window column;
     * the pill is raised into view with a pure scene offset. Floats over the
     * content without taking layout space or intercepting clicks elsewhere
     * (unlike an overlay layer, which hit-tests across the whole window). */
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    ui_set_size(sz_grow(), (struct layout_size){ SIZE_FIXED, 0, 0, 0, 0 });
    ui_set_offset(0, -62.0f + (1.0f - op) * 16.0f);   /* rise above the tab bar */
    ui_set_opacity(op < 1.0f ? op : 1.0f);
    {
        ui_begin_hstack(1);
        ui_set_paint(solid(t->text));                 /* dark pill, light text (modern toast) */
        ui_set_corner_radius(t->radius_pill);
        ui_set_shadow(true, t->shadow_lg.dx, t->shadow_lg.dy, t->shadow_lg.blur, t->shadow_lg.color);
        ui_set_padding(t->sp2, t->sp4, t->sp2, t->sp4);
        ui_set_align(ALIGN_CENTER);
        ui_set_spacing(t->sp2);
        { EmProps ip = { .font = BodyBold, .color = accent }; em_icon_impl(cp, ip); }
        { EmProps mp = { .font = Body, .color = t->bg }; em_text_impl(g_toast.msg ? g_toast.msg : "", mp); }
        ui_end_stack();
    }
    ui_end_stack();
}

/* ---- SearchField: field + leading search glyph + trailing clear --------- */
static bool em_search_impl(char *buf, size_t cap, const char *ph, bool *out_hov) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered();
    if (out_hov) *out_hov = hov;
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_pill);
    ui_set_border(1.0f, hov ? t->border_strong : t->border);
    ui_set_padding(t->sp2, t->sp3, t->sp2, t->sp3);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    ui_set_size(sz_grow(), sz_intrinsic());
    { EmProps ip = { .font = Body, .color = t->text_tertiary }; em_icon_impl(IconMagnify, ip); }
    /* the actual editable field, borderless, grows */
    { EmProps fp = { .grow = 1 }; (void)fp; em_field_impl(buf, cap, ph, (EmProps){ .grow = 1 }, 0); }
    if (buf && buf[0]) {
        EmProps xp = { .font = Body, .color = t->text_tertiary };
        if (em_iconbtn_impl(IconClose, xp, 0)) buf[0] = '\0';   /* clear */
    }
    ui_end_stack();
    return ui_consume_click(self);
}

/* ---- Disclosure: a tappable header revealing its scope children -------- */
static int g_disc_open;   /* passed from em_disclosure_ to _end_ */
void em_disclosure_(const char *title, bool *open, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);                 /* wrapper: header + (optional) body */
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp2);
    em_apply_box(p);
    /* header row (tappable) */
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    ui_set_paint(solid(pressed ? shade(t->surface_alt, 0.94f) : hov ? t->surface_alt : t->surface));
    ui_set_corner_radius(t->radius_md);
    ui_set_border(1.0f, t->border);
    ui_set_padding(t->sp3, t->sp4, t->sp3, t->sp4);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp3);
    { EmProps tp = { .font = BodyBold, .color = t->text }; em_text_impl(title, tp); }
    ui_spacer();
    { EmProps cp = { .font = Body, .color = t->text_secondary };
      em_icon_impl((open && *open) ? IconChevronU : IconChevronD, cp); }
    ui_end_stack();
    if (ui_consume_click(self) && open) { *open = !*open; g_em_epoch++; }
    g_disc_open = (open && *open) ? 1 : 0;
    if (!g_disc_open) return;           /* body suppressed: children still emit but into a hidden box */
    ui_begin_vstack(0);                 /* the body -- children append here */
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp2);
    ui_set_padding(0, t->sp2, t->sp2, t->sp2);
}
void em_disclosure_end_(void) {
    em_flush();
    if (g_disc_open) ui_end_stack();    /* close body */
    ui_end_stack();                     /* close wrapper */
}

/* ---- Dropdown / Picker ------------------------------------------------- */
/* One dropdown open at a time, keyed by the sel pointer. The menu is drawn
 * inline right under the field (a simple, robust anchor -- no overlay needed
 * for a first cut; it participates in normal layout/scroll). */
static const int *g_dd_open;   /* which dropdown (by sel ptr) is expanded */

static bool em_dropdown_impl(const char *const *labels, int count, int *sel, bool *out_hov) {
    const struct ui_theme *t = TH;
    int cur = sel ? *sel : 0;
    if (cur < 0) cur = 0;
    if (cur >= count) cur = count - 1;
    bool is_open = (g_dd_open == (const int *)sel);

    ui_begin_vstack(0);                 /* field + (optional) menu */
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp1);

    /* the field */
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    if (out_hov) *out_hov = hov;
    ui_set_paint(solid(pressed ? shade(t->surface_alt, 0.94f) : hov ? t->surface_alt : t->surface));
    ui_set_corner_radius(t->radius_md);
    ui_set_border(1.0f, is_open ? t->accent : t->border);
    ui_set_padding(t->sp2 + 1, t->sp3, t->sp2 + 1, t->sp3);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    ui_set_size(sz_grow(), sz_intrinsic());
    { EmProps vp = { .font = Body, .color = t->text }; em_text_impl(count > 0 ? labels[cur] : "", vp); }
    ui_spacer();
    { EmProps cp = { .font = Body, .color = t->text_secondary };
      em_icon_impl(is_open ? IconChevronU : IconChevronD, cp); }
    ui_end_stack();
    if (ui_consume_click(self)) {
        g_dd_open = is_open ? 0 : (const int *)sel;   /* toggle */
        is_open = !is_open;
        g_em_epoch++;
    }

    /* the menu (inline, under the field) */
    if (is_open) {
        ui_begin_vstack(0);
        ui_set_paint(solid(t->surface));
        ui_set_corner_radius(t->radius_md);
        ui_set_border(1.0f, t->border);
        ui_set_clip_children(true);
        ui_set_align(ALIGN_STRETCH);
        ui_set_spacing(0);
        ui_set_shadow(true, t->shadow_md.dx, t->shadow_md.dy, t->shadow_md.blur, t->shadow_md.color);
        for (int i = 0; i < count; i++) {
            ui_begin_hstack((uint64_t)(i + 1));
            struct instance_handle row = ui_open();
            bool rhov = ui_is_hovered(), rpr = ui_is_pressed();
            ui_set_paint(solid(rpr ? shade(t->accent_soft, 0.94f)
                                   : rhov ? t->accent_soft
                                          : (i == cur ? t->surface_alt : t->surface)));
            ui_set_padding(t->sp2 + 1, t->sp3, t->sp2 + 1, t->sp3);
            ui_set_align(ALIGN_CENTER);
            ui_set_spacing(t->sp2);
            { EmProps lp = { .font = Body, .color = (i == cur) ? t->accent : t->text }; em_text_impl(labels[i], lp); }
            ui_spacer();
            if (i == cur) { EmProps ck = { .font = Body, .color = t->accent }; em_icon_impl(IconCheck, ck); }
            ui_end_stack();
            if (ui_consume_click(row)) {
                if (sel) *sel = i;
                g_dd_open = 0; is_open = 0;
                g_em_epoch++;
            }
        }
        ui_end_stack();
    }
    ui_end_stack();
    return false;
}

/* ---- TabView: page + bottom tab bar with an eased selection pill -------- */
void em_tabview(int *sel, const EmTab *items, int count) {
    em_flush();
    const struct ui_theme *t = TH;
    int cur = sel ? *sel : 0;
    if (cur < 0) cur = 0;
    if (cur >= count) cur = count - 1;

    ui_begin_vstack(0);
    ui_set_size(sz_grow(), sz_grow());
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(0);

    /* the active page fills the space above the bar */
    ui_begin_vstack(0);
    ui_set_size(sz_grow(), sz_grow());
    ui_set_align(ALIGN_STRETCH);
    em_flush();
    if (count > 0 && items[cur].page) items[cur].page();
    em_flush();
    ui_end_stack();

    /* the bottom tab bar */
    ui_begin_hstack(0);
    ui_set_paint(solid(t->surface));
    ui_set_border(1.0f, t->border);
    ui_set_padding(t->sp1, t->sp2, t->sp1, t->sp2);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_SPACE_BETWEEN);
    ui_set_size(sz_grow(), sz_intrinsic());
    for (int i = 0; i < count; i++) {
        ui_begin_vstack((uint64_t)(i + 1));
        struct instance_handle tab = ui_open();
        bool hov = ui_is_hovered();
        int on = (i == cur);
        Color fg = on ? t->accent : (hov ? t->text_secondary : t->text_tertiary);
        ui_set_paint(solid(on ? t->accent_soft : t->surface));
        ui_set_corner_radius(t->radius_md);
        ui_set_padding(t->sp1, t->sp3, t->sp1, t->sp3);
        ui_set_align(ALIGN_CENTER);
        ui_set_spacing(2);
        ui_set_size(sz_grow(), sz_intrinsic());
        { char g[5]; utf8_enc(items[i].icon, g);
          ui_set_font(t->font_regular); ui_set_text_size(t->text_body_lg);
          ui_set_text_color(fg); ui_text("%s", g); }
        { ui_set_font(t->font_bold); ui_set_text_size(t->text_caption);
          ui_set_text_color(fg); ui_text("%s", items[i].label); }
        ui_end_stack();
        if (ui_consume_click(tab) && sel && *sel != i) { *sel = i; g_em_epoch++; }
    }
    ui_end_stack();     /* bar */
    ui_end_stack();     /* tabview */
}

/* ---- SplitView: fixed sidebar surface + growing content ---------------- */
static float g_split_w;
void em_split_(float sidebar_w, EmProps p) {
    em_flush();
    g_split_w = sidebar_w > 0 ? sidebar_w : 220.0f;
    ui_begin_hstack(0);
    ui_set_size(sz_grow(), sz_grow());
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(0);
    em_apply_box(p);
}
void em_split_end_(void) { em_flush(); ui_end_stack(); }
void em_sidebar_(EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    ui_set_paint(solid(p.background.a > 0 ? p.background : t->surface));
    ui_set_border(1.0f, t->border);
    ui_set_size(sz_fixed(g_split_w), sz_grow());
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp1);
    ui_set_padding(t->sp3, t->sp2, t->sp3, t->sp2);
    em_apply_box(p);
}
void em_content_(EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    ui_set_size(sz_grow(), sz_grow());
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp3);
    ui_set_padding(t->sp5, t->sp5, t->sp5, t->sp5);
    em_apply_box(p);
}

/* ======================================================================= */
/* resources (V4.1) -- path-keyed caches over an injected loader           */
/* ======================================================================= */

static uint8_t *(*g_res_load)(const char *path, size_t *out_len);
void em_res_set_loader(uint8_t *(*load)(const char *path, size_t *out_len)) {
    g_res_load = load;
}

#define EM_RES_MAX 8

uint32_t em_font(const char *path) {
    static struct { const char *path; uint32_t handle; } cache[EM_RES_MAX];
    static int installed;
    if (!path) return 0;
    for (int i = 0; i < EM_RES_MAX; i++)
        if (cache[i].path && strcmp(cache[i].path, path) == 0) return cache[i].handle;
    if (!g_res_load) return 0;
    size_t len = 0;
    uint8_t *data = g_res_load(path, &len);       /* kept alive: font parses in place */
    uint32_t h = (data && len) ? font_load(data, len) : 0;
    if (h && !installed) { font_install_backend(); installed = 1; }
    for (int i = 0; i < EM_RES_MAX; i++)
        if (!cache[i].path) { cache[i].path = path; cache[i].handle = h; break; }
    return h;
}

/* Minimal P6 (binary) .ppm decoder into malloc'd BGRA-premul, cached by path. */
const uint32_t *em_image(const char *path, uint32_t *out_w, uint32_t *out_h) {
    static struct { const char *path; uint32_t *px, w, h; } cache[EM_RES_MAX];
    if (!path) return 0;
    for (int i = 0; i < EM_RES_MAX; i++)
        if (cache[i].path && strcmp(cache[i].path, path) == 0) {
            if (out_w) *out_w = cache[i].w;
            if (out_h) *out_h = cache[i].h;
            return cache[i].px;
        }
    if (!g_res_load) return 0;
    size_t len = 0;
    uint8_t *d = g_res_load(path, &len);
    if (!d || len < 16 || d[0] != 'P' || d[1] != '6') return 0;
    size_t o = 2; uint32_t vals[3] = {0,0,0}; int nv = 0;
    while (o < len && nv < 3) {                        /* width height maxval */
        while (o < len && (d[o]==' '||d[o]=='\n'||d[o]=='\r'||d[o]=='\t')) o++;
        if (o < len && d[o] == '#') { while (o < len && d[o] != '\n') o++; continue; }
        uint32_t v = 0; int any = 0;
        while (o < len && d[o] >= '0' && d[o] <= '9') { v = v*10 + (d[o]-'0'); o++; any = 1; }
        if (!any) return 0;
        vals[nv++] = v;
    }
    o++;                                               /* single whitespace after maxval */
    uint32_t w = vals[0], h = vals[1];
    if (!w || !h || vals[2] == 0 || o + (size_t)w*h*3 > len) return 0;
    uint32_t *px = (uint32_t *)malloc((size_t)w*h*4);
    if (!px) return 0;
    for (size_t i = 0; i < (size_t)w*h; i++) {
        uint8_t r = d[o+i*3], g = d[o+i*3+1], b = d[o+i*3+2];
        px[i] = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | b;  /* opaque = premul */
    }
    for (int i = 0; i < EM_RES_MAX; i++)
        if (!cache[i].path) { cache[i].path = path; cache[i].px = px; cache[i].w = w; cache[i].h = h; break; }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return px;
}

void em_image_view(const char *path, EmProps p) {
    em_flush();
    uint32_t w = 0, h = 0;
    const uint32_t *px = em_image(path, &w, &h);
    if (!px) {   /* missing resource: a quiet placeholder box, not a crash */
        ui_box_begin(0);
        ui_set_paint(solid(TH->surface_alt));
        ui_set_corner_radius(TH->radius_md);
        ui_set_size(sz_grow(), sz_fixed(p.height > 0 ? p.height : 64));
        ui_box_end();
        return;
    }
    ui_image((uint64_t)(uintptr_t)px, px, w, h, p.height > 0 ? p.height : (float)h);
}

void em_theme_use(EmTheme t) { ui_theme_use_dark(t != Light); }

/* ======================================================================= */
/* EmUI V6 -- menus (menu bar, dropdown menus, context menus)              */
/* ======================================================================= */

/* One menu is open at a time, keyed by the Menu's label pointer. The open
 * menu's items float in an out-of-flow overlay anchored where the button was
 * clicked (ui_pointer_pos at open time -- no layout query needed). */
static const void *g_menu_open;          /* label ptr of the open Menu, or NULL */
static float g_menu_ax, g_menu_ay;       /* anchor (window-content coords) */
static int   g_menu_cur_open;            /* is the Menu being emitted right now open? */
static int   g_menu_item_chosen;         /* a MenuItem was clicked this frame */
static struct instance_handle g_menu_scrim;

/* right-click edge, fed by em_feed_right_button (em_app_run). */
static int   g_rclick_pending;
static float g_rclick_x, g_rclick_y;
static int   g_rbtn_prev;

void em_feed_right_button(float x, float y, bool down) {
    if (down && !g_rbtn_prev) { g_rclick_pending = 1; g_rclick_x = x; g_rclick_y = y; }
    g_rbtn_prev = down ? 1 : 0;
}
int em_right_clicked(float *ox, float *oy) {
    if (!g_rclick_pending) return 0;
    g_rclick_pending = 0;
    if (ox) *ox = g_rclick_x;
    if (oy) *oy = g_rclick_y;
    return 1;
}

void em_menubar_(EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    ui_set_paint(solid(p.background.a > 0 ? p.background : t->surface_alt));
    ui_set_border(0, t->border);
    ui_set_padding(t->sp1, t->sp2, t->sp1, t->sp2);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp1);
    ui_set_size(sz_grow(), sz_intrinsic());
    em_apply_box(p);
}
void em_menubar_end_(void) { em_flush(); ui_end_stack(); }

/* Shared: open the floating popover panel for the currently-open menu at
 * (ax, ay). Items emit into it; em_*_menu_end_ closes it + handles dismiss. */
static void em_menu_panel_open(uint64_t key, float ax, float ay) {
    const struct ui_theme *t = TH;
    ui_begin_vstack(key);                 /* the out-of-flow overlay layer */
    g_menu_scrim = ui_open();
    ui_set_overlay(true);
    ui_set_paint(solid((Color){0,0,0,0}));        /* transparent: catches outside clicks, no dim */
    ui_set_size(sz_grow(), sz_grow());
    ui_begin_vstack(1);                   /* the menu panel -- frosted glass */
    ui_set_offset(ax, ay);
    ui_set_corner_radius(t->radius_md);
    ui_set_shadow(true, t->shadow_lg.dx, t->shadow_lg.dy, t->shadow_lg.blur, t->shadow_lg.color);
    em_glass_apply(12.0f);                 /* blur behind + tint + edge highlight */
    ui_set_clip_children(true);
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(0);
    ui_set_padding(t->sp1, t->sp1, t->sp1, t->sp1);
    ui_set_size(sz_fixed(200), sz_intrinsic());
}
/* returns 1 if the transparent scrim (outside the panel) was clicked */
static int em_menu_panel_close(void) {
    ui_end_stack();                       /* panel */
    int scrim_hit = ui_consume_click(g_menu_scrim);
    ui_end_stack();                       /* overlay */
    return scrim_hit;
}
/* the hidden container a CLOSED menu's items emit into (built but invisible) */
static void em_menu_hidden_open(void) {
    ui_begin_vstack(0);
    ui_set_size(sz_fixed(0), sz_fixed(0));
    ui_set_clip_children(true);
}

void em_menu_(const char *label, EmProps p) {
    (void)p;
    em_flush();
    const struct ui_theme *t = TH;
    int is_open = (g_menu_open == (const void *)label);
    ui_begin_hstack(0);                   /* the menu button in the bar */
    struct instance_handle btn = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    ui_set_paint(solid(is_open ? t->accent_soft : pressed ? shade(t->surface_alt, 0.94f)
                                : hov ? t->surface_alt : t->surface_alt));
    if (!is_open && !hov && !pressed) ui_set_paint(solid((Color){0,0,0,0}));   /* flat until hovered */
    ui_set_corner_radius(t->radius_sm);
    ui_set_padding(t->sp1, t->sp3, t->sp1, t->sp3);
    ui_set_align(ALIGN_CENTER);
    { EmProps lp = { .font = Body, .color = is_open ? t->accent : t->text }; em_text_impl(label, lp); }
    ui_end_stack();
    if (ui_consume_click(btn)) {
        if (is_open) g_menu_open = 0;
        else { g_menu_open = (const void *)label; float px, py; ui_pointer_pos(&px, &py);
               g_menu_ax = px; g_menu_ay = py + 8.0f; }
        g_em_epoch++;
        is_open = (g_menu_open == (const void *)label);
    }
    g_menu_cur_open = is_open;
    if (is_open) em_menu_panel_open((uint64_t)(uintptr_t)label, g_menu_ax, g_menu_ay);
    else         em_menu_hidden_open();
}
void em_menu_end_(void) {
    em_flush();
    if (g_menu_cur_open) { if (em_menu_panel_close()) { g_menu_open = 0; g_em_epoch++; } }
    else ui_end_stack();                  /* hidden box */
}

bool em_menu_item(const char *label, const char *shortcut) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    ui_set_paint(solid(pressed ? shade(t->accent_soft, 0.94f) : hov ? t->accent_soft : t->surface));
    ui_set_corner_radius(t->radius_sm);
    ui_set_padding(t->sp2, t->sp3, t->sp2, t->sp3);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp3);
    ui_set_size(sz_grow(), sz_intrinsic());
    { EmProps lp = { .font = Body, .color = hov ? t->accent : t->text }; em_text_impl(label, lp); }
    if (shortcut && shortcut[0]) {
        ui_spacer();
        EmProps sp = { .font = Caption, .color = t->text_tertiary }; em_text_impl(shortcut, sp);
    }
    ui_end_stack();
    if (ui_consume_click(self)) { g_menu_open = 0; g_menu_item_chosen = 1; g_em_epoch++; return true; }
    return false;
}

void em_menu_separator(void) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_box_begin(0);
    ui_set_paint(solid(t->border));
    ui_set_size(sz_grow(), sz_fixed(1));
    ui_box_end();
}

/* ContextMenu: a popover at (x,y) while *open; item-click or outside-click
 * clears *open. Same panel machinery as Menu. g_menu_item_chosen (set by
 * MenuItem on click) is the "an item was picked" signal both Menu and
 * ContextMenu use to dismiss. */
static int   g_ctx_cur_open;
static bool *g_ctx_open_flag;
void em_context_menu_(bool *open, float x, float y, EmProps p) {
    (void)p;
    em_flush();
    g_ctx_cur_open = (open && *open) ? 1 : 0;
    g_ctx_open_flag = open;
    g_menu_item_chosen = 0;               /* fresh: only THIS frame's item clicks count */
    if (g_ctx_cur_open) em_menu_panel_open((uint64_t)(uintptr_t)open, x, y);
    else                em_menu_hidden_open();
}
void em_context_menu_end_(void) {
    em_flush();
    if (g_ctx_cur_open) {
        int scrim_hit = em_menu_panel_close();
        if ((scrim_hit || g_menu_item_chosen) && g_ctx_open_flag) {
            *g_ctx_open_flag = false;
            g_em_epoch++;
        }
    } else {
        ui_end_stack();                   /* hidden box */
    }
}

/* ======================================================================= */
/* EmUI V7 -- multi-line text editor                                       */
/* ======================================================================= */

/* Private key codes the kernel keyboard driver emits for the extended nav keys
 * (kept in lockstep with kernel/drivers/input/keyboard.c EK_* and embk.h
 * EMBK_KEY_*). Defined here so em.c stays SDK-free and host-testable. */
#define EMK_LEFT  0x11
#define EMK_RIGHT 0x12
#define EMK_UP    0x13
#define EMK_DOWN  0x14
#define EMK_HOME  0x02
#define EMK_END   0x05
#define EMK_DEL   0x7F

/* ---- editing primitives on a NUL-terminated buffer + byte cursor -------- */
static void te_insert(char *buf, size_t cap, int *len, int *cur, char c) {
    if (*len + 1 >= (int)cap) return;
    memmove(buf + *cur + 1, buf + *cur, (size_t)(*len - *cur + 1));  /* incl. NUL */
    buf[*cur] = c;
    (*cur)++; (*len)++;
}
static void te_backspace(char *buf, int *len, int *cur) {
    if (*cur <= 0) return;
    memmove(buf + *cur - 1, buf + *cur, (size_t)(*len - *cur + 1));
    (*cur)--; (*len)--;
}
static void te_delete(char *buf, int *len, int *cur) {
    if (*cur >= *len) return;
    memmove(buf + *cur, buf + *cur + 1, (size_t)(*len - *cur));
    (*len)--;
}
static int te_line_start(const char *buf, int cur) {
    int i = cur;
    while (i > 0 && buf[i - 1] != '\n') i--;
    return i;
}
static int te_line_end(const char *buf, int len, int cur) {
    int i = cur;
    while (i < len && buf[i] != '\n') i++;
    return i;
}

bool em_text_editor(char *buf, size_t cap, int *cursor, float height) {
    em_flush();                       /* emit any pending staged leaf first */
    const struct ui_theme *t = TH;
    int len = (int)strlen(buf);
    int cur = cursor ? *cursor : 0;
    if (cur > len) cur = len;
    if (cur < 0) cur = 0;

    ui_begin_vstack(0);
    struct instance_handle self = ui_open();
    if (ui_consume_click(self)) ui_request_focus(self);
    bool focused = ui_has_focus(self);

    if (focused) {
        char in[64];
        int n = ui_input_take(in, (int)sizeof in);
        for (int i = 0; i < n; i++) {
            unsigned char c = (unsigned char)in[i];
            switch (c) {
                case '\b':      te_backspace(buf, &len, &cur); break;
                case EMK_DEL:   te_delete(buf, &len, &cur); break;
                case '\n': case '\r': te_insert(buf, cap, &len, &cur, '\n'); break;
                case '\t':      te_insert(buf, cap, &len, &cur, ' ');
                                te_insert(buf, cap, &len, &cur, ' '); break;
                case EMK_LEFT:  if (cur > 0) cur--; break;
                case EMK_RIGHT: if (cur < len) cur++; break;
                case EMK_HOME:  cur = te_line_start(buf, cur); break;
                case EMK_END:   cur = te_line_end(buf, len, cur); break;
                case EMK_UP: {
                    int ls = te_line_start(buf, cur), col = cur - ls;
                    if (ls > 0) { int pls = te_line_start(buf, ls - 1), ple = ls - 1;
                                  cur = pls + (col < ple - pls ? col : ple - pls); }
                    break;
                }
                case EMK_DOWN: {
                    int ls = te_line_start(buf, cur), col = cur - ls;
                    int le = te_line_end(buf, len, cur);
                    if (le < len) { int nls = le + 1, nle = te_line_end(buf, len, nls);
                                    cur = nls + (col < nle - nls ? col : nle - nls); }
                    break;
                }
                default:
                    if (c >= 32 && c < 127) te_insert(buf, cap, &len, &cur, (char)c);
                    break;
            }
        }
        if (cursor) *cursor = cur;
        em_request_frame();   /* keep the loop live while typing */
    }

    /* the editor surface */
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_md);
    ui_set_border(focused ? 1.5f : 1.0f, focused ? t->accent : t->border);
    ui_set_padding(t->sp2, t->sp3, t->sp2, t->sp3);
    ui_set_clip_children(true);
    ui_set_align(ALIGN_STRETCH);
    ui_set_size(sz_grow(), sz_fixed(height));

    /* auto-scroll: shift the lines up so the cursor line stays visible */
    float line_h = t->text_body + 5.0f;
    int cur_line = 0; for (int i = 0; i < cur; i++) if (buf[i] == '\n') cur_line++;
    float caret_y = (cur_line + 1) * line_h;
    float view_h = height - 2 * t->sp2;
    float scroll = caret_y > view_h ? caret_y - view_h : 0.0f;

    ui_begin_vstack(0);
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(5);
    ui_set_offset(0, -scroll);

    if (len == 0 && !focused) {
        EmProps pp = { .font = Body, .color = t->text_tertiary };
        em_text_impl("Type here...", pp);
    } else {
        /* render each line; the cursor line splits around a caret box */
        int line = 0, i = 0;
        while (i <= len) {
            int e = i; while (e < len && buf[e] != '\n') e++;
            int is_cur = (cur >= i && cur <= e);
            char tmp[512];
            if (is_cur && focused) {
                ui_begin_hstack((uint64_t)(line + 1));
                ui_set_align(ALIGN_CENTER);
                ui_set_spacing(0);
                int bn = cur - i; if (bn > (int)sizeof tmp - 1) bn = sizeof tmp - 1;
                memcpy(tmp, buf + i, (size_t)bn); tmp[bn] = 0;
                /* skip empty text nodes: an empty string in an ALIGN_CENTER hstack
                 * collapses the row's layout (same reason the plain-line path below
                 * substitutes a space). Cursor at line start -> no before-text;
                 * cursor at line end -> no after-text; the caret always draws. */
                if (bn > 0) { EmProps tp = { .font = Body, .color = t->text }; em_text_impl(tmp, tp); }
                ui_box_begin(0);              /* caret */
                ui_set_paint(solid(t->accent));
                ui_set_size(sz_fixed(2), sz_fixed(t->text_body));
                ui_box_end();
                int an = e - cur; if (an > (int)sizeof tmp - 1) an = sizeof tmp - 1;
                memcpy(tmp, buf + cur, (size_t)an); tmp[an] = 0;
                if (an > 0) { EmProps tp = { .font = Body, .color = t->text }; em_text_impl(tmp, tp); }
                ui_spacer();
                ui_end_stack();
            } else {
                int ln = e - i; if (ln > (int)sizeof tmp - 1) ln = sizeof tmp - 1;
                memcpy(tmp, buf + i, (size_t)ln); tmp[ln] = 0;
                ui_begin_hstack((uint64_t)(line + 1));
                ui_set_align(ALIGN_CENTER);
                { EmProps tp = { .font = Body, .color = t->text }; em_text_impl(tmp[0] ? tmp : " ", tp); }
                ui_spacer();
                ui_end_stack();
            }
            line++;
            i = e + 1;
            if (e == len) break;
        }
    }
    ui_end_stack();     /* lines */
    ui_end_stack();     /* editor surface */
    return focused;
}
