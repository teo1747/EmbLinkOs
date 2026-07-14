/* ui/dsl/showcase_v2.c -- renders an app screen written in the EmUI V2 DSL to a
 * PNG. This is what an app author writes: SwiftUI-flavored, declarative, bound
 * to plain C state. `make showcase-v2` builds + renders it (light + dark). */

#include "em.h"
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

/* ---- app state (SwiftUI's @State -- here just plain variables) ---------- */
static bool  g_dark = true, g_notify = true, g_wifi = false;
static int   g_theme_sel = 0;
static float g_volume = 0.7f;
static int   g_devices = 3;
static char  g_name[32] = "Ada Lovelace";

/* A reusable component is just a C function that emits views. Take parameters
 * like any function; call it wherever you'd repeat the UI. */
static void StatRow(int icon, const char *label, const char *value) {
    Row() {
        Label(icon, label);
        Spacer();
        Text(value).caption().secondary();
    }
}

/* ---- the view -- everything below is chainable modifiers + bindings ----- */
static void app(void) {
    static const char *const themes[] = { "Auto", "Light", "Dark" };

    Screen(.justify = Center, .align = Center) {
        Card(.width = 400, .spacing = 16) {

            NavBar("Account") {
                IconButton(IconGear).id("settings");
            }

            /* profile header -- note the chained .title() / .caption().secondary() */
            HStack(.spacing = 14) {
                Avatar("AL");
                VStack(.spacing = 2, .align = Leading) {
                    Text("Ada Lovelace").title();
                    Text("ada@emblink.os").caption().secondary();
                }
                Spacer();
                Badge("Pro").success();
            }

            Banner(IconCheck, "All changes saved.").success();

            Divider();

            Section("PROFILE") {
                Text("Display name").caption().secondary();
                TextField(g_name, sizeof g_name, "Your name");
            }

            Section("PREFERENCES") {
                Toggle("Dark mode", &g_dark);
                Toggle("Notifications", &g_notify);
                Toggle("Wi-Fi sync", &g_wifi);
                Row() { Text("Theme"); Spacer(); }
                Segmented(themes, 3, &g_theme_sel);
                Row() { Text("Volume"); Spacer(); Text("70%").caption().secondary(); }
                Slider(&g_volume);
                Stepper("Devices", &g_devices, 1, 10);
            }

            Section("TAGS") {
                HStack(.spacing = 6) {
                    Tag("Design").accent();
                    Tag("Active").success();
                    Tag("Beta").warning();
                    Spacer();
                }
            }

            Section("STORAGE") {
                StatRow(IconBolt, "Used", "7.2 / 10 GB");   /* reusable component */
                ProgressBar(0.72f);
            }

            Section("ACTIVITY") {
                static const float week[] = { 3, 5, 2, 7, 4, 8, 6 };
                Chart(week, 7, .height = 72);
            }

            Section("TREND") {
                static const float trend[] = { 4, 6, 5, 9, 7, 8, 6, 11, 9, 12 };
                AreaChart(trend, 10, .height = 96);
            }

            Section("QUICK ACTIONS") {
                List() {
                    ListRow(IconStar, "Starred", "12").id("starred");
                    ListRow(IconBell, "Alerts", "3").id("alerts");
                }
            }

            Divider();

            HStack(.spacing = 10) {
                Spacer();
                if (Button("Cancel").secondary().clicked()) {}
                if (Button("Save changes").primary().clicked()) {}
            }
        }
    }
}

/* ======================================================================= */
/* V4 gallery -- every EmUI V4 addition on one canvas ("v4" as argv[3]).    */
/* App-owned chrome: this is exactly what a chromeless OS window shows --   */
/* the Window surface, its OWN WindowBar, and the custom CloseButton.       */
/* ======================================================================= */

static int   g_tab = 0;
static int   g_dd_sel = 1;
static bool  g_adv_open = true;                 /* render the Disclosure expanded */
static char  g_query[32] = "kernel";            /* non-empty -> clear button shows */

static void V4Dashboard(void) {
    static const float cpu[]  = { 3, 5, 4, 7, 6, 9, 8, 11 };
    static const float mem[]  = { 8, 7, 9, 6, 7, 5, 6, 4 };
    VStack(.spacing = 14, .padding = 20, .align = Fill) {
        HStack(.spacing = 12, .align = Fill) {
            StatCard("CPU", "37%", "+2.1%", cpu, 8);
            StatCard("MEMORY", "1.2 GB", "-0.4%", mem, 8);
        }
        HStack(.spacing = 24) {
            Gauge(0.72f, "72%");
            VStack(.spacing = 10, .align = Leading) {
                Text("Disk usage").heading();
                Text("7.2 of 10 GB used").caption().secondary();
                HStack(.spacing = 8) { Spinner(); Text("Syncing...").caption().secondary(); }
            }
        }
        DividerLabel("CONTROLS");
        { static const char *const envs[] = { "Development", "Staging", "Production" };
          Dropdown(envs, 3, &g_dd_sel); }
        SearchField(g_query, sizeof g_query, "Search anything");
        Disclosure("Advanced", &g_adv_open) {
            Toggle("Verbose logging", &g_notify);
            Toggle("Telemetry", &g_wifi);
        }
    }
}

static void V4Files(void) {
    Split(180) {
        SidebarPane() {
            ListRow(IconFiles, "Documents", "12").id("s1");
            ListRow(IconDot,  "Recent",     "4").id("s2");
            ListRow(IconClose,  "Trash",       "").id("s3");
        }
        ContentPane() {
            EmptyState(IconCloud, "No files here", "Drop something in, or sync a device.");
        }
    }
}

static void v4app(void) {
    static const EmTab tabs[] = {
        { IconGrid, "Dashboard", V4Dashboard },
        { IconFiles, "Files",   V4Files },
    };
    Window("EmUI V4") {
        WindowBar("EmUI V4") {
            if (CloseButton().clicked()) { /* the app decides: quit, hide, confirm */ }
        }
        TabView(&g_tab, tabs, 2);
        ToastHost();     /* floats over everything; em_toast() raised it below */
    }
}

/* V6 gallery: a menu bar + an open context-menu popover (rendered statically
 * via a forced-open ContextMenu, so a still frame shows the menu system). */
static void v6app(void) {
    static bool ctx = true;   /* forced open for the static render */
    Window("EmUI V6 - Menus") {
        MenuBar() {
            Menu("File") {}
            Menu("Edit") {}
            Menu("View") {}
            Menu("Help") {}
        }
        VStack(.padding = 24, .spacing = 8) {
            Text("Right-click anywhere for a context menu.").secondary();
            Text("Menus float above content and dismiss on outside-click.").caption().tertiary();
        }
        ContextMenu(&ctx, 120, 150) {
            if (MenuItemK("New",  "Ctrl+N")) {}
            if (MenuItemK("Open", "Ctrl+O")) {}
            MenuSeparator();
            if (MenuItemK("Save", "Ctrl+S")) {}
            if (MenuItem("Save As...")) {}
            MenuSeparator();
            if (MenuItem("Preferences")) {}
            if (MenuItemK("Quit", "Ctrl+Q")) {}
        }
    }
}

/* V7 gallery: a multi-line text editor with content, shown in a window. */
static void v7app(void) {
    static char doc[2048] =
        "EmUI V7 - multi-line text editor\n"
        "\n"
        "Type to insert, Enter for a newline,\n"
        "Backspace / Delete to remove.\n"
        "Arrow keys, Home and End move the cursor.\n"
        "The view auto-scrolls to keep the caret visible.";
    static int cur = 0;
    Window("EmUI V7 - Editor") {
        VStack(.padding = 20, .spacing = 12, .align = Fill) {
            Text("Notes").heading();
            TextEditor(doc, sizeof doc, &cur, 260);
            HStack(.spacing = 10) {
                Spacer();
                Text("multi-line, arrow-key cursor").caption().tertiary();
            }
        }
    }
}

/* Glass gallery: a frosted panel floating over rich, colorful content, so the
 * backdrop blur + tint + edge highlight are all visible ("g" as argv[3]). */
static void gapp(void) {
    static const float wave[] = { 3, 6, 4, 9, 7, 11, 8, 12, 9, 13 };
    static const float bars[] = { 5, 8, 4, 9, 6, 7 };
    Screen(.padding = 0) {
        /* busy, high-frequency background (color blocks + text + charts) so the
         * blur behind the glass is actually visible, not just translucency. */
        VStack(.align = Fill, .spacing = 0, .grow = 1) {
            HStack(.align = Fill, .spacing = 0, .grow = 1) {
                VStack(.background = (Color){0.18f,0.26f,0.82f,1}, .grow = 1, .padding = 18, .align = Leading) {
                    Text("EmbLink OS").title(); Text("frosted glass").caption(); }
                VStack(.background = (Color){0.74f,0.24f,0.52f,1}, .grow = 1, .padding = 18, .align = Leading) {
                    Text("Depth").title(); AreaChart(wave, 10, .height = 76); }
            }
            HStack(.align = Fill, .spacing = 0, .grow = 1) {
                VStack(.background = (Color){0.12f,0.52f,0.52f,1}, .grow = 1, .padding = 18, .align = Leading) {
                    Text("Blur").heading(); Chart(bars, 6, .height = 72); }
                VStack(.background = (Color){0.86f,0.54f,0.18f,1}, .grow = 1, .padding = 18, .align = Leading) {
                    Text("Material").heading(); Text("backdrop + tint + edge").caption(); }
            }
        }
    }
    /* the frosted glass panel, floated over the color field via the overlay
     * primitive (a Dialog with .glass = 1), so it blurs what is behind it */
    Overlay() {
        Dialog(.glass = 1, .width = 360, .padding = 24, .spacing = 12) {
            Text("Glass").title();
            Text("Frosted backdrop blur, EmbLink tint, edge highlight.")
                .caption().secondary();
            AreaChart(wave, 10, .height = 84);
        }
    }
}

int main(int argc, char **argv) {
    int W = 480, H = 1080;
    const char *out = argc > 1 ? argv[1] : "v2.ppm";
    bool dark = (argc > 2 && argv[2][0] == 'd');
    bool v4   = (argc > 3 && argv[3][0] == 'v');
    bool v6   = (argc > 3 && argv[3][0] == '6');
    bool v7   = (argc > 3 && argv[3][0] == '7');
    bool gl   = (argc > 3 && argv[3][0] == 'g');
    if (v4) { W = 660; H = 900; }
    if (v6) { W = 560; H = 420; }
    if (v7) { W = 560; H = 420; }
    if (gl) { W = 620; H = 460; }

    size_t rl = 0, bl = 0;
    uint8_t *reg  = read_file("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", &rl);
    uint8_t *bold = read_file("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", &bl);
    if (!reg || !bold) { fprintf(stderr, "could not load DejaVu fonts\n"); return 1; }
    uint32_t fr = font_load(reg, rl), fb = font_load(bold, bl);
    if (!fr || !fb) { fprintf(stderr, "font_load failed\n"); return 1; }
    font_install_backend();

    struct scene_arena sa; scene_arena_init(&sa);
    struct layout_arena la; layout_arena_init(&la);
    ui_theme_use_dark(dark);
    ui_theme_set_fonts(fr, fb);
    ui_init(&sa, &la);

    if (v4) em_toast("Snapshot created", Success);   /* host clock is 0 -> stays visible */
    ui_frame_begin(); em_new_frame(); if (gl) gapp(); else if (v7) v7app(); else if (v6) v6app(); else if (v4) v4app(); else app(); em_flush(); ui_frame_end();
    ui_run_layout((float)W, (float)H);

    struct render_target rt;
    rt.pixels = malloc((size_t)W * H * 4); rt.width = W; rt.height = H; rt.stride = W * 4;
    rt.format = EMBK_PIXFMT_BGRA8888_PRE;
    const struct ui_theme *t = ui_theme();
    uint8_t br = (uint8_t)(t->bg.b * 255), bgc = (uint8_t)(t->bg.g * 255), rr = (uint8_t)(t->bg.r * 255);
    for (int i = 0; i < W * H; i++)
        ((uint32_t *)rt.pixels)[i] = (255u << 24) | ((uint32_t)rr << 16) | ((uint32_t)bgc << 8) | br;

    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);

    FILE *f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint32_t px = ((uint32_t *)rt.pixels)[i];
        uint8_t bb = px & 255, gg = (px >> 8) & 255, rrr = (px >> 16) & 255;
        uint8_t rgb[3] = { rrr, gg, bb };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "wrote %s (%dx%d)\n", out, W, H);
    return 0;
}
