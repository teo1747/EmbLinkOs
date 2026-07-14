/* user/bin/v6demo.c -- the EmUI V6 reference app: menus.
 *
 * Runs in a CHROMELESS compositor window. Shows the V6 menu system live:
 *   - a MenuBar with File / Edit / View / Help drop-down Menus (click to open,
 *     click an item or outside to dismiss);
 *   - a right-click ContextMenu anchored where the pointer was pressed.
 * The EmApplication runtime feeds the right mouse button into the toolkit, so
 * RightClicked() fires on a right-press anywhere in the content. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

static const char *g_last = "(none)";
static bool  g_ctx = false;
static float g_cx = 120, g_cy = 140;

static void app(void) {
    Window("Menus") {
        WindowBar("Menus") {
            CloseGrip();   /* pull to dismiss -- the runtime handles teardown */
        }
        MenuBar() {
            Menu("File") {
                if (MenuItemK("New",  "Ctrl+N")) g_last = "New";
                if (MenuItemK("Open", "Ctrl+O")) g_last = "Open";
                MenuSeparator();
                if (MenuItemK("Quit", "Ctrl+Q")) g_last = "Quit";
            }
            Menu("Edit") {
                if (MenuItemK("Undo", "Ctrl+Z")) g_last = "Undo";
                if (MenuItemK("Redo", "Ctrl+Y")) g_last = "Redo";
                MenuSeparator();
                if (MenuItemK("Cut",  "Ctrl+X")) g_last = "Cut";
                if (MenuItemK("Copy", "Ctrl+C")) g_last = "Copy";
                if (MenuItemK("Paste","Ctrl+V")) g_last = "Paste";
            }
            Menu("View") {
                if (MenuItem("Zoom In"))  g_last = "Zoom In";
                if (MenuItem("Zoom Out")) g_last = "Zoom Out";
                if (MenuItem("Reset"))    g_last = "Reset";
            }
            Menu("Help") {
                if (MenuItem("Documentation")) g_last = "Documentation";
                if (MenuItem("About"))         g_last = "About";
            }
        }
        VStack(.padding = 24, .spacing = 10, .align = Leading) {
            Text("Menus").heading();
            Text("Click a menu above, or right-click anywhere here.").secondary();
            HStack(.spacing = 6) {
                Text("Last action:").caption().secondary();
                Text(g_last).caption();
            }
        }
        if (RightClicked(&g_cx, &g_cy)) g_ctx = true;
        ContextMenu(&g_ctx, g_cx, g_cy) {
            if (MenuItemK("New",  "Ctrl+N")) g_last = "ctx New";
            if (MenuItemK("Open", "Ctrl+O")) g_last = "ctx Open";
            MenuSeparator();
            if (MenuItem("Preferences"))     g_last = "ctx Preferences";
        }
    }
}

EM_APPLICATION {
    .title  = "Menus",
    .size   = { 560, 420 },
    .theme  = Dark,
    .chrome = Chromeless,
    .resize = Resizable,
    .view   = app,
};
