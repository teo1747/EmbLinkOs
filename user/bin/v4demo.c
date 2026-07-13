/* user/bin/v4demo.c -- the EmUI V4 reference app.
 *
 * Everything V4 in one place, running in a CHROMELESS compositor window: the
 * kernel draws NO title bar and NO close button -- the app owns its chrome.
 * Its WindowBar drags the window (embk_win_move via em's registered mover) and
 * its own round CloseButton quits it. Content is a TabView (Dashboard /
 * Controls / About) exercising every new component: StatCard, Gauge, Spinner,
 * Toast, Dropdown, SearchField, Disclosure, DividerLabel, Split, EmptyState. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

/* ---- app state ---------------------------------------------------------- */
static int   g_tab = 0;
static int   g_env = 1;
static bool  g_adv = false, g_verbose = true, g_telemetry = false;
static char  g_query[48] = "";
static float g_load = 0.72f;

/* ---- pages -------------------------------------------------------------- */
static void Dashboard(void) {
    static const float cpu[] = { 3, 5, 4, 7, 6, 9, 8, 11 };
    static const float mem[] = { 8, 7, 9, 6, 7, 5, 6, 4 };
    VStack(.spacing = 12, .padding = 18, .align = Fill) {
        HStack(.spacing = 12, .align = Fill) {
            StatCard("CPU", "37%", "+2.1%", cpu, 8);
            StatCard("MEMORY", "1.2 GB", "-0.4%", mem, 8);
        }
        HStack(.spacing = 20) {
            Gauge(g_load, "72%", .height = 104);
            VStack(.spacing = 8, .align = Leading) {
                Text("Disk usage").heading();
                Text("7.2 of 10 GB used").caption().secondary();
                HStack(.spacing = 8) { Spinner(); Text("Syncing...").caption().secondary(); }
                if (Button("Snapshot").primary().clicked())
                    em_toast("Snapshot created", Success);
            }
        }
    }
}

static void Controls(void) {
    static const char *const envs[] = { "Development", "Staging", "Production" };
    VStack(.spacing = 12, .padding = 18, .align = Fill) {
        DividerLabel("ENVIRONMENT");
        Dropdown(envs, 3, &g_env);
        SearchField(g_query, sizeof g_query, "Search anything");
        Disclosure("Advanced", &g_adv) {
            Toggle("Verbose logging", &g_verbose);
            Toggle("Telemetry", &g_telemetry);
        }
    }
}

static void About(void) {
    Split(190) {
        SidebarPane() {
            ListRow(IconFiles, "Components", "10").id("a1");
            ListRow(IconBolt,  "Perf",        "").id("a2");
            ListRow(IconStar,  "Credits",     "").id("a3");
        }
        ContentPane() {
            EmptyState(IconCloud, "EmUI V4", "App-owned chrome, tabs, split views and more.");
        }
    }
}

static void app(void) {
    static const EmTab tabs[] = {
        { IconGrid,  "Dashboard", Dashboard },
        { IconGear,  "Controls",  Controls },
        { IconInfo,  "About",     About },
    };
    Window("V4 Demo") {
        WindowBar("V4 Demo") {
            if (CloseButton().clicked()) { /* observed via em_window_closed() in the loop */ }
        }
        TabView(&g_tab, tabs, 3);
        ToastHost();
    }
}

/* That's the whole app: the EmApplication runtime does the rest -- resources
 * (font via the embk loader), arenas/theme, the CHROMELESS window + WindowBar
 * drag wiring, the RETAINED event loop (idle frames run no UI work at all),
 * dirty-rect presents, and teardown via our own CloseButton or ESC. */
EM_APPLICATION {
    .title  = "V4 Demo",
    .size   = { 700, 560 },
    .theme  = Dark,
    .chrome = Chromeless,
    .resize = Resizable,
    .view   = app,
};
