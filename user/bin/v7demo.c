/* user/bin/v7demo.c -- the EmUI V7 reference app: a multi-line text editor.
 *
 * Runs in a CHROMELESS compositor window (the app owns its chrome via
 * WindowBar + CloseButton). The EmApplication runtime grabs the keyboard and
 * feeds every key into the toolkit, so TextEditor gets typing, Enter, Tab,
 * Backspace/Delete and the arrow/Home/End navigation keys (the kernel emits
 * those as EMBK_KEY_* codes). The view auto-scrolls to keep the caret on
 * screen. Click the editor to focus it, then type. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

static char g_doc[4096] =
    "EmUI V7 -- multi-line text editor\n"
    "\n"
    "Click here to focus, then type.\n"
    "Enter makes a new line, Tab inserts two spaces,\n"
    "Backspace and Delete remove, and the arrow keys,\n"
    "Home and End move the caret. The view scrolls to\n"
    "follow the cursor.\n";
static int g_cur = 0;

static void app(void) {
    Window("Editor") {
        WindowBar("Editor") {
            CloseGrip();   /* pull to dismiss -- the runtime handles teardown */
        }
        VStack(.spacing = 10, .padding = 16, .align = Fill) {
            Text("Notes").heading();
            Text("A live, focusable, scrolling text editor.").caption().secondary();
            TextEditor(g_doc, sizeof g_doc, &g_cur, 320);
        }
    }
}

EM_APPLICATION {
    .title    = "Editor",
    .size     = { 560, 460 },
    .theme    = Dark,
    .chrome   = Chromeless,
    .resize   = Resizable,
    .view     = app,
};
