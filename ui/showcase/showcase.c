/* ui/showcase/showcase.c -- render a real EmbLink UI to an image.
 *
 * Composes a settings card with the themed widget kit, runs layout + the CPU
 * backend against real DejaVu Sans/Bold fonts (also the Piece-4b real-font
 * integration check), and writes a PPM. `make showcase` converts it to PNG.
 * Not a unit test -- the point is to SEE that the toolkit produces clean UI. */

#include "kit.h"
#include "ui.h"
#include "theme.h"
#include "scene_render.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return 0; }
    fclose(f); *len = (size_t)n; return buf;
}

/* the app UI -- this is exactly what an app author writes */
static bool g_dark_mode = true;
static bool g_reduce_motion = false;
static bool g_check = true;
static int  g_radio = 1;
static int  g_tab = 0;
static float g_volume = 0.65f;
static void app(void) {
    static const char *const tabs[] = { "Display", "Sound", "Network" };

    ui_screen_begin();
    ui_set_justify(JUSTIFY_CENTER);
    ui_set_align(ALIGN_CENTER);

    ui_card_begin(1);
        struct layout_size fixed360 = { SIZE_FIXED, 360, 0, 0, 0 };
        struct layout_size intrinsic = { SIZE_INTRINSIC, 0, 0, 0, 0 };
        ui_set_size(fixed360, intrinsic);

        ui_row_begin(0);
            ui_avatar("EM");
            ui_col_begin(0);
                ui_title("Settings");
                ui_secondary("Signed in as embl");
            ui_col_end();
            ui_flex_spacer();
            ui_badge("Pro", BADGE_SUCCESS);
        ui_row_end();

        g_tab = ui_segmented(tabs, 3, g_tab);

        ui_divider();

        ui_row_begin(0);
            ui_body("Dark mode");
            ui_flex_spacer();
            if (ui_toggle(g_dark_mode)) g_dark_mode = !g_dark_mode;
        ui_row_end();
        ui_row_begin(0);
            if (ui_checkbox(g_check)) g_check = !g_check;
            ui_body("Enable animations");
            ui_flex_spacer();
        ui_row_end();
        ui_row_begin(0);
            if (ui_radio(g_radio == 0)) g_radio = 0;
            ui_body("Light");
            if (ui_radio(g_radio == 1)) g_radio = 1;
            ui_body("Auto");
            ui_flex_spacer();
        ui_row_end();

        ui_row_begin(0);
            ui_body("Volume");
            ui_flex_spacer();
        ui_row_end();
        g_volume = ui_slider(g_volume);

        ui_row_begin(0);
            ui_caption("Sync");
            ui_flex_spacer();
            ui_caption("72%%");
        ui_row_end();
        ui_progress(0.72f);

        ui_row_begin(0);
            ui_chip("All", true);
            ui_chip("Unread", false);
            ui_chip("Starred", false);
            ui_flex_spacer();
        ui_row_end();

        ui_divider();

        ui_row_begin(0);
            ui_flex_spacer();
            ui_button_secondary("Cancel");
            ui_button_primary("Save changes");
        ui_row_end();
    ui_card_end();

    ui_screen_end();
}

int main(int argc, char **argv) {
    int W = 520, H = 640;
    const char *out = argc > 1 ? argv[1] : "showcase.ppm";
    bool dark = (argc > 2 && argv[2][0] == 'd');

    /* real fonts (also the Piece-4b integration check) */
    size_t rl = 0, bl = 0;
    uint8_t *reg = read_file("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", &rl);
    uint8_t *bold = read_file("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", &bl);
    if (!reg || !bold) { fprintf(stderr, "could not load DejaVu fonts\n"); return 1; }
    uint32_t fr = font_load(reg, rl), fb = font_load(bold, bl);
    if (!fr || !fb) { fprintf(stderr, "font_load failed\n"); return 1; }
    font_install_backend();   /* wire draw_text into the CPU backend vtable */

    struct scene_arena sa; scene_arena_init(&sa);
    struct layout_arena la; layout_arena_init(&la);
    ui_theme_use_dark(dark);
    ui_theme_set_fonts(fr, fb);
    ui_init(&sa, &la);

    ui_frame_begin(); app(); ui_frame_end();
    ui_run_layout((float)W, (float)H);

    /* clear to bg, then paint the retained scene */
    struct render_target rt;
    rt.pixels = malloc((size_t)W * H * 4); rt.width = W; rt.height = H; rt.stride = W * 4;
    rt.format = EMBK_PIXFMT_BGRA8888_PRE;
    const struct ui_theme *t = ui_theme();
    uint8_t br = (uint8_t)(t->bg.b*255), bgc = (uint8_t)(t->bg.g*255), rr = (uint8_t)(t->bg.r*255);
    for (int i = 0; i < W*H; i++)
        ((uint32_t*)rt.pixels)[i] = (255u<<24)|((uint32_t)rr<<16)|((uint32_t)bgc<<8)|br;

    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);

    /* write PPM (P6, straight RGB -- pixels are opaque after compositing on bg) */
    FILE *f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W*H; i++) {
        uint32_t px = ((uint32_t*)rt.pixels)[i];
        uint8_t bb = px & 255, gg = (px>>8)&255, rrr = (px>>16)&255;
        uint8_t rgb[3] = { rrr, gg, bb };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "wrote %s (%dx%d)\n", out, W, H);
    return 0;
}
