#ifndef __EMBLINK_UI_THEME_H__
#define __EMBLINK_UI_THEME_H__

/* ui/theme/theme.h -- EmbLink UI: the design-token system.
 *
 * A curated set of semantic tokens (color roles, a spacing grid, a type scale,
 * radii, shadow presets) that make clean UI the DEFAULT. The widget kit
 * (ui/kit) reads exclusively from these tokens, so an app author gets a
 * coherent look without picking a single raw colour -- and restyling the whole
 * OS is one call, ui_theme_use_dark().
 *
 * Aesthetic: restrained, modern, dev-tool -- cool-biased neutrals, hairline
 * borders, soft layered shadows, a single confident indigo accent, generous
 * spacing on an 8px grid. Hierarchy comes from size + weight + colour, not
 * decoration. */

#include <stdint.h>
#include "scene.h"   /* struct color */

struct ui_shadow_spec { float dx, dy, blur; struct color color; };

struct ui_theme {
    /* --- surfaces & lines --- */
    struct color bg;             /* app background */
    struct color surface;        /* cards, panels */
    struct color surface_alt;    /* insets, subtle fills (input wells, hovers) */
    struct color border;         /* hairline dividers / card edges */
    struct color border_strong;  /* emphasised edges, focus rings */

    /* --- text (three-step hierarchy) --- */
    struct color text;           /* primary */
    struct color text_secondary; /* labels, supporting copy */
    struct color text_tertiary;  /* captions, disabled, metadata */

    /* --- accent (spend boldness here only) --- */
    struct color accent;
    struct color accent_hover;
    struct color accent_soft;    /* tinted background for accent surfaces/badges */
    struct color on_accent;      /* text/icon on an accent fill */

    /* --- semantic status (separate from the accent) --- */
    struct color success, warning, danger;

    /* --- radii --- */
    float radius_sm, radius_md, radius_lg, radius_pill;

    /* --- spacing scale (8px grid; sp1 is the 4px half-step) --- */
    float sp1, sp2, sp3, sp4, sp5, sp6, sp7;   /* 4 8 12 16 24 32 48 */

    /* --- type scale (px) --- */
    float text_caption, text_body, text_body_lg, text_title, text_heading;

    /* --- elevation --- */
    struct ui_shadow_spec shadow_sm, shadow_md, shadow_lg;

    /* --- fonts (set by the app after loading its .ttf files) --- */
    uint32_t font_regular, font_bold;
};

/* The active theme. Never NULL. */
const struct ui_theme *ui_theme(void);

/* Swap light/dark (preserves the font handles). */
void ui_theme_use_dark(bool dark);

/* Register the loaded font handles the kit uses for regular vs. bold text. */
void ui_theme_set_fonts(uint32_t regular, uint32_t bold);

#endif /* __EMBLINK_UI_THEME_H__ */
