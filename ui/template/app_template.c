/* ui/template/app_template.c -- EmbLink UI: a starter app template.
 *
 * Copy this file, rename app()/AppState, and build your screen inside app().
 * It is deliberately small: the whole point of the toolkit is that a clean,
 * consistent UI falls out of composing kit widgets -- you never touch a raw
 * colour, size, or font.
 *
 * ---------------------------------------------------------------------------
 * THE MENTAL MODEL (read once)
 * ---------------------------------------------------------------------------
 *   - You DECLARE your whole UI every frame, top to bottom, by calling widgets.
 *     Re-declaring identical UI is free -- the toolkit diffs it and does no
 *     work. So you never "create" or "update" widgets by hand; you just
 *     describe what should be on screen right now, given your state.
 *   - State lives in plain variables (or, for reactive updates, signals).
 *     A control returns an event (e.g. a button returns `true` the frame it
 *     was clicked); you mutate your state in response, and next frame's
 *     declaration reflects it.
 *   - Layout is flexbox: rows (hstack) and columns (vstack), with spacing,
 *     padding, alignment, and flexible spacers. You give sizes as FIXED,
 *     INTRINSIC (fit content), or FLEX (grow/shrink to share space).
 *   - Every colour/size/radius comes from the theme (ui/theme). Switch the
 *     whole OS between light and dark with ui_theme_use_dark(true/false).
 * ---------------------------------------------------------------------------
 */

#include "kit.h"     /* cards, rows, labels, buttons, toggles, badges */
#include "ui.h"      /* the raw declarative API (ui_box_begin, ui_set_*, ...) */
#include "theme.h"   /* ui_theme(), ui_theme_use_dark(), tokens */

/* 1. Your app's state is just data. */
struct app_state {
    bool  notifications;
    bool  auto_update;
    int   click_count;
};
static struct app_state S = { .notifications = true, .auto_update = false };

/* 2. Your screen. Called once per frame. Describe what should be visible now. */
void app(void) {
    ui_screen_begin();                 /* full-window page, themed background */
    ui_set_justify(JUSTIFY_CENTER);    /* center the card vertically ... */
    ui_set_align(ALIGN_CENTER);        /* ... and horizontally */

    ui_card_begin(1);                  /* an elevated surface (border + shadow) */
        struct layout_size w360 = { SIZE_FIXED, 360, 0, 0, 0 };
        struct layout_size fit  = { SIZE_INTRINSIC, 0, 0, 0, 0 };
        ui_set_size(w360, fit);

        /* header: a title on the left, a status badge pushed to the right */
        ui_row_begin(0);
            ui_title("Settings");
            ui_flex_spacer();
            ui_badge(S.auto_update ? "Up to date" : "Update ready",
                     S.auto_update ? BADGE_SUCCESS : BADGE_WARNING);
        ui_row_end();
        ui_secondary("Manage how this device behaves.");

        ui_divider();

        /* a settings row: label -> spacer -> toggle. Clicking flips state. */
        ui_row_begin(0);
            ui_body("Notifications");
            ui_flex_spacer();
            if (ui_toggle(S.notifications)) S.notifications = !S.notifications;
        ui_row_end();
        ui_row_begin(0);
            ui_body("Automatic updates");
            ui_flex_spacer();
            if (ui_toggle(S.auto_update)) S.auto_update = !S.auto_update;
        ui_row_end();

        ui_divider();

        /* footer actions. ui_button_* returns true the frame it is clicked. */
        ui_row_begin(0);
            ui_caption("Saved %d times", S.click_count);
            ui_flex_spacer();
            if (ui_button_secondary("Reset"dle frames now have nd=0, render dropped from ~2.4 billion cycles → ~200k ()) { S.notifications = true; S.auto_update = false; }
            if (ui_button_primary("Save"))    { S.click_count++; }
        ui_row_end();
    ui_card_end();

    ui_screen_end();
}

/* 3. Wiring (do this once at startup). In a real app on EmbLinkOS this lives in
 * your process's main(): create a window/surface (Piece 1/2), load a font,
 * then run the frame loop -- drain input -> reactivity_flush -> ui_frame_*/app
 * -> ui_run_layout -> paint -> commit. See ui/showcase/showcase.c for a
 * complete, runnable host example that renders app() to an image. */
