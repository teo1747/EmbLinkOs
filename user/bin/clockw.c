/* user/bin/clockw.c -- the EmbLink CLOCK desktop widget (EmUI V5).
 *
 * A tiny always-on-desktop tile: big uptime clock + a live activity spark.
 * Sits in the widget z-band (above the wallpaper, below every app window);
 * home spawns it at boot. The whole program is a view + one EM_WIDGET. */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

static float g_spark[12];
static int   g_spark_n;

static void ClockView(void) {
    uint64_t ms = embk_uptime_ms();
    uint64_t secs = ms / 1000;
    static char t1[16], t2[24];
    snprintf(t1, sizeof t1, "%lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    snprintf(t2, sizeof t2, "uptime  ·  EmbLink");

    /* a wandering spark so the widget visibly lives */
    if (g_spark_n < 12) g_spark[g_spark_n++] = (float)((ms / 250) % 9) + 1.0f;
    else { for (int i = 0; i < 11; i++) g_spark[i] = g_spark[i + 1]; g_spark[11] = (float)((ms / 250) % 9) + 1.0f; }

    Window("Clock", .glass = 1, .corner = 16) {   /* translucent glass tint + edge highlight */
        VStack(.spacing = 2, .padding = 12, .align = Leading) {
            Text(t1).title();
            Text(t2).caption().tertiary();
            if (g_spark_n >= 2) { AreaChart(g_spark, g_spark_n, .height = 26); }
        }
    }
}

EM_WIDGET {
    .title      = "Clock",
    .size       = { 190, 108 },
    .pos        = { 24, 24 },
    .theme      = Dark,
    .refresh_ms = 1000,
    .view       = ClockView,
};
