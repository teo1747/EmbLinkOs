/* ==========================================================================
 * hist.c -- see hist.h. A ring of fixed slots; same shape as the terminal's
 * scrollback ring (head = next write slot, count saturates).
 * ========================================================================== */
#include "hist/hist.h"
#include <string.h>

static char   g_hist[HIST_MAX][HIST_LINE_MAX];
static size_t g_head;    /* next slot to write */
static size_t g_count;   /* live entries, saturates at HIST_MAX */

/* logical index (0 = oldest) -> ring slot */
static size_t slot_of(size_t i) {
    return (g_head - g_count + i + 2 * HIST_MAX) % HIST_MAX;
}

void hist_push(const char *line) {
    if (!line || line[0] == '\0')
        return;   /* never store an empty line */

    /* ignoredups: collapse a repeat of the most recent entry */
    if (g_count > 0) {
        const char *prev = g_hist[slot_of(g_count - 1)];
        if (strcmp(prev, line) == 0)
            return;
    }

    size_t n = strlen(line);
    if (n >= HIST_LINE_MAX)
        n = HIST_LINE_MAX - 1;
    memcpy(g_hist[g_head], line, n);
    g_hist[g_head][n] = '\0';

    g_head = (g_head + 1) % HIST_MAX;
    if (g_count < HIST_MAX)
        g_count++;   /* else: the oldest just fell off, by construction */
}

size_t hist_count(void) { return g_count; }

const char *hist_get(size_t i) {
    if (i >= g_count)
        return 0;
    return g_hist[slot_of(i)];
}
