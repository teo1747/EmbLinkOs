#ifndef __EMBLINK_EM_UI_H__
#define __EMBLINK_EM_UI_H__

/* ui/dsl/em.h -- EmUI V2: a SwiftUI-flavored declarative DSL for EmbLink.
 *
 * A thin macro + function layer over the immediate-mode core (ui/declare) that
 * makes app UI read like SwiftUI:
 *
 *     VStack(.spacing = 12, .padding = 20, .background = T.surface, .corner = 16) {
 *         HStack(.align = Center) {
 *             Text("Settings", .font = Title);
 *             Spacer();
 *             Badge("Pro", .tone = Success);
 *         }
 *         Divider();
 *         Toggle("Dark mode", &g_dark);
 *         Slider(&g_volume);
 *         if (Button("Save changes", .style = Primary)) save();
 *     }
 *
 * How the syntax works in C:
 *   - Containers are brace scopes via a for-loop guard (VStack { ... }).
 *   - Modifiers are C designated initializers into one EmProps struct -- the
 *     direct analog of SwiftUI's named arguments. 0 means "unset -> default".
 *   - Views bind to state through pointers (&g_dark), SwiftUI's $binding.
 *
 * RULE: do NOT `return`, `break`, `continue`, or `goto` out of a container
 * block -- it skips the matching close and unbalances the tree (same caveat as
 * every C UI-scope macro). Structure views so control flow stays inside. */

#include "ui.h"
#include "theme.h"
#include <stddef.h>
#include <stdint.h>

typedef struct color Color;

/* ------------------------------------------------------------------------- */
/* enums (short, SwiftUI-like names; 0 == default)                           */
/* ------------------------------------------------------------------------- */

typedef enum {                 /* .font = Title / Body / ... */
    FontDefault = 0, Body, BodyBold, Title, Heading, Caption
} EmFont;

typedef enum {                 /* .align / .justify (shared) */
    AlignDefault = 0, Leading, Center, Trailing, Fill, SpaceBetween
} EmAlign;

typedef enum {                 /* .style on Button */
    StyleDefault = 0, Primary, Secondary, Ghost, Destructive
} EmStyle;

typedef enum {                 /* .tone on Badge / Tag / Banner */
    ToneDefault = 0, Accent, Success, Warning, Danger, Neutral
} EmTone;

/* ------------------------------------------------------------------------- */
/* EmProps -- every view accepts these as designated-init modifiers.          */
/* A zero field means "not set"; the view falls back to a sensible default.   */
/* ------------------------------------------------------------------------- */

typedef struct {
    /* layout */
    float spacing;                       /* gap between children */
    float padding, px, py;               /* all / horizontal / vertical */
    float pt, pr, pb, pl;                /* per-edge overrides */
    float width, height;                 /* fixed size (0 = intrinsic) */
    int   grow;                          /* fill available space (main axis) */

    /* surface */
    Color background, color, border_color;
    float border, corner;
    int   shadow;                        /* 0 none, 1 sm, 2 md, 3 lg */
    float opacity;                       /* 0 -> treated as 1 */
    int   clip;                          /* clip children to bounds */
    int   glass;                         /* frosted backdrop-blur material */
    float blur;                          /* glass blur radius (0 -> theme default) */

    /* text */
    EmFont font;

    /* alignment */
    EmAlign align, justify;

    /* variants */
    EmStyle style;
    EmTone  tone;
} EmProps;

/* ------------------------------------------------------------------------- */
/* design tokens -- `T.accent`, `T.text`, ... resolve from the active theme.  */
/* ------------------------------------------------------------------------- */

typedef struct {
    Color accent, accent_soft, on_accent;
    Color text, secondary, tertiary;
    Color surface, surface_alt, bg;
    Color border, border_strong;
    Color success, warning, danger;
    Color clear;                         /* transparent */
} EmTokens;
const EmTokens *em_tokens_(void);
#define T (*em_tokens_())

/* ------------------------------------------------------------------------- */
/* the brace-scope guard: `Container(...) { children }`                       */
/* ------------------------------------------------------------------------- */

#define EM_CAT_(a, b) a##b
#define EM_CAT2_(a, b) EM_CAT_(a, b)
#define EM_SCOPE_(open, close) \
    for (int EM_CAT2_(_em_, __LINE__) = ((open), 0); \
         EM_CAT2_(_em_, __LINE__) == 0; \
         EM_CAT2_(_em_, __LINE__) = ((close), 1))

/* ------------------------------------------------------------------------- */
/* containers (brace-scoped)                                                  */
/* ------------------------------------------------------------------------- */

#define VStack(...) EM_SCOPE_(em_vstack_((EmProps){__VA_ARGS__}), em_end_())
#define HStack(...) EM_SCOPE_(em_hstack_((EmProps){__VA_ARGS__}), em_end_())
#define ZStack(...) EM_SCOPE_(em_zstack_((EmProps){__VA_ARGS__}), em_end_())
#define Card(...)   EM_SCOPE_(em_card_((EmProps){__VA_ARGS__}),  em_end_())
/* Glass: a frosted panel -- blurs whatever is behind it (backdrop blur),
 * tinted with the theme surface + a hint of the EmbLink accent, and finished
 * with a light edge highlight for depth. Equivalent to any container with
 * `.glass = 1` (optionally `.blur = <radius>`). Use for chrome, menus, sheets. */
#define Glass(...)  EM_SCOPE_(em_glass_((EmProps){__VA_ARGS__}),  em_end_())
#define Screen(...) EM_SCOPE_(em_screen_((EmProps){__VA_ARGS__}),em_end_())
#define Section(title, ...) EM_SCOPE_(em_section_((title), (EmProps){__VA_ARGS__}), em_end_())
#define ScrollView(bind, height, ...) EM_SCOPE_(em_scroll_((bind), (height), (EmProps){__VA_ARGS__}), em_scroll_end_())

void em_vstack_(EmProps p);
void em_hstack_(EmProps p);
void em_zstack_(EmProps p);
void em_card_(EmProps p);
void em_glass_(EmProps p);
void em_screen_(EmProps p);
void em_section_(const char *title, EmProps p);
void em_scroll_(float *scroll_y, float viewport_h, EmProps p);
void em_scroll_end_(void);
void em_end_(void);

#define NavBar(title, ...)  EM_SCOPE_(em_navbar_((title), (EmProps){__VA_ARGS__}), em_end_())
#define Row(...)            EM_SCOPE_(em_row_((EmProps){__VA_ARGS__}), em_end_())
void em_navbar_(const char *title, EmProps p);
void em_row_(EmProps p);

/* ------------------------------------------------------------------------- */
/* leaves + controls -- CHAINABLE. A leaf stages a pending element and returns  */
/* the chain object; modifiers mutate it; it is emitted at the next boundary.   */
/*                                                                              */
/*     Text("Hello").caption().secondary();                                     */
/*     if (Button("Save").primary().clicked()) save();                          */
/*     Button("Delete").destructive().id("del");   // + if (Clicked("del"))     */
/* ------------------------------------------------------------------------- */

typedef struct EmV EmV;
struct EmV {
    /* type roles (text) */
    EmV (*title)(void);   EmV (*heading)(void); EmV (*body)(void);
    EmV (*bold)(void);    EmV (*caption)(void); EmV (*font)(EmFont);
    /* colour -- kind-aware (text colour, or a control's variant/tone) */
    EmV (*color)(Color);
    EmV (*secondary)(void); EmV (*tertiary)(void); EmV (*accent)(void);
    EmV (*primary)(void);   EmV (*ghost)(void);    EmV (*destructive)(void);
    EmV (*tone)(EmTone);    EmV (*success)(void);  EmV (*warning)(void); EmV (*danger)(void);
    /* box */
    EmV (*bg)(Color);     EmV (*padding)(float); EmV (*px)(float); EmV (*py)(float);
    EmV (*frame)(float, float); EmV (*width)(float); EmV (*height)(float); EmV (*grow)(void);
    EmV (*corner)(float); EmV (*border)(float);  EmV (*shadow)(int);
    EmV (*center)(void);  EmV (*leading)(void);  EmV (*trailing)(void); EmV (*align)(EmAlign);
    /* identity + interaction terminals */
    EmV  (*id)(const char *);
    bool (*clicked)(void);
    bool (*focused)(void);
};

#define Text(...)        em_text(__VA_ARGS__)
#define Icon(...)        em_icon(__VA_ARGS__)
#define Label(...)       em_label(__VA_ARGS__)
#define Badge(...)       em_badge(__VA_ARGS__)
#define Tag(...)         em_tag(__VA_ARGS__)
#define Avatar(...)      em_avatar(__VA_ARGS__)
#define Banner(...)      em_banner(__VA_ARGS__)
#define ProgressBar(...) em_progress(__VA_ARGS__)
#define Button(...)      em_button(__VA_ARGS__)
#define IconButton(...)  em_icon_button(__VA_ARGS__)
#define Toggle(...)      em_toggle(__VA_ARGS__)
#define Checkbox(...)    em_checkbox(__VA_ARGS__)
#define Slider(...)      em_slider(__VA_ARGS__)
#define Stepper(...)     em_stepper(__VA_ARGS__)
#define TextField(...)   em_text_field(__VA_ARGS__)
#define Segmented(...)   em_segmented(__VA_ARGS__)
#define Spacer()         em_spacer_()
#define Divider()        em_divider_()

EmV em_text(const char *s);
EmV em_icon(int codepoint);
EmV em_label(int codepoint, const char *s);
EmV em_badge(const char *s);
EmV em_tag(const char *s);
EmV em_avatar(const char *initials);
EmV em_banner(int codepoint, const char *msg);
EmV em_progress(float frac);
EmV em_button(const char *s);
EmV em_icon_button(int codepoint);
EmV em_toggle(const char *label, bool *bind);
EmV em_checkbox(const char *label, bool *bind);
EmV em_slider(float *bind);
EmV em_stepper(const char *label, int *bind, int lo, int hi);
EmV em_text_field(char *buf, size_t cap, const char *placeholder);
EmV em_segmented(const char *const *labels, int count, int *bind);
void em_spacer_(void);
void em_divider_(void);

/* --- richer components --- */
/* Chart: a mini bar chart (values scaled to the max; last bar emphasised). */
#define Chart(vals, n, ...)  em_chart((vals), (n), (EmProps){__VA_ARGS__})
void em_chart(const float *vals, int n, EmProps p);

/* LineChart / AreaChart: a smooth polyline (software-rasterised via a real line
 * primitive) over a bitmap; AreaChart also fills beneath it. One on screen at a
 * time, fixed data. `.height`/`.color` supported. */
#define LineChart(vals, n, ...)  em_linechart((vals), (n), 0, (EmProps){__VA_ARGS__})
#define AreaChart(vals, n, ...)  em_linechart((vals), (n), 1, (EmProps){__VA_ARGS__})
void em_linechart(const float *vals, int n, int filled, EmProps p);

/* List: a grouped, inset surface; ListRow: a tappable row with a trailing
 * chevron. ListRow is chainable (.id / .clicked). */
#define List(...)     EM_SCOPE_(em_list_((EmProps){__VA_ARGS__}), em_list_end_())
#define ListRow(...)  em_listrow(__VA_ARGS__)
void em_list_(EmProps p);
void em_list_end_(void);
EmV  em_listrow(int icon, const char *title, const char *value);

/* ======================================================================= */
/* EmUI V4                                                                  */
/* ======================================================================= */

/* --- App-owned window chrome (the custom close) ------------------------- *
 * A V4 app opens a CHROMELESS OS window (embk_win_create_shared_ex with
 * EMBK_WINF_CHROMELESS) -- no kernel title bar or close button -- and draws
 * its OWN chrome with these. Window() is the full-bleed top-level surface;
 * WindowBar() is a draggable strip whose trailing scope holds the app's own
 * controls (a CloseButton, menus, whatever). Everything is normal toolkit
 * nodes, so the whole bar restyles through EmProps/theme.
 *
 *   Window("Files") {
 *       WindowBar("Files") { if (CloseButton().clicked()) quit(); }
 *       ... app content ...
 *   }
 *
 * Register how the window moves + its id/pos ONCE at startup (keeps the
 * toolkit free of any syscall dependency, exactly like em_set_clock):
 *   em_window_set_mover(my_move);   // my_move calls embk_win_move
 *   em_window_bind(win_id, x, y);   // initial screen position                */
#define Window(title, ...)    EM_SCOPE_(em_window_((title), (EmProps){__VA_ARGS__}), em_window_end_())
#define WindowBar(title, ...) EM_SCOPE_(em_windowbar_((title), (EmProps){__VA_ARGS__}), em_windowbar_end_())
void em_window_(const char *title, EmProps p);
void em_window_end_(void);
void em_windowbar_(const char *title, EmProps p);
void em_windowbar_end_(void);
#define CloseButton(...)  em_close_button()
EmV  em_close_button(void);            /* modern single round close control; chainable (.clicked()) */
int  em_window_closed(void);           /* 1 if the built-in CloseButton fired this frame */
/* CloseGrip: EmbLink's own close GESTURE (not a fixed button). Put it in the
 * WindowBar; the user PULLS it -- the window fades + slides toward the drag and
 * closes once pulled past the threshold (springs back if released early), the
 * same drag-to-commit shape as the resize corner grip. The EM_APPLICATION
 * runtime handles the actual teardown (via em_window_take_close). */
#define CloseGrip()  em_close_grip()
bool em_close_grip(void);
int  em_window_take_close(void);       /* 1 the frame the close gesture committed */
int  em_window_pulling(void);          /* 1 while a close pull is animating (fade/slide) */
void em_window_set_mover(void (*mover)(int win, int32_t x, int32_t y));
void em_window_bind(int win, int32_t x, int32_t y);

/* Resizable windows (V5): the runtime enables the grip; Window() then draws a
 * corner handle whose drag, ON RELEASE, records a size delta the runtime
 * collects with em_window_take_resize and applies via embk_win_resize. */
void em_window_set_resizable(int on);
void em_window_set_glass(int on);   /* Window() renders a translucent tint for glass */
int  em_window_take_resize(int *dw, int *dh);   /* 1 if a resize is pending */

/* --- new components ----------------------------------------------------- */
/* Dropdown / Picker: a field showing labels[*sel]; tap to open a menu, tap an
 * item to pick it. Manages its own open/closed state (one open at a time). */
EmV  em_dropdown(const char *const *labels, int count, int *sel);
#define Dropdown(labels, count, sel)  em_dropdown((labels), (count), (sel))

/* Toast: transient message; call em_toast() to raise one, ToastHost() once at
 * the root (LAST, so it floats on top) to render whatever is active. */
void em_toast(const char *msg, EmTone tone);
void em_toast_host(void);
#define ToastHost()  em_toast_host()

/* Spinner: indeterminate activity (phase-animated dots). Gauge: ring progress
 * 0..1, rasterised (like LineChart). SearchField: text field + search/clear. */
EmV  em_spinner(void);
#define Spinner(...)  em_spinner()
void em_gauge(float frac, const char *center, EmProps p);
#define Gauge(frac, center, ...)  em_gauge((frac), (center), (EmProps){__VA_ARGS__})
EmV  em_search_field(char *buf, size_t cap, const char *placeholder);
#define SearchField(buf, cap, ph)  em_search_field((buf), (cap), (ph))

/* Disclosure: a tappable header that shows/hides its scope children. */
#define Disclosure(title, open, ...) \
    EM_SCOPE_(em_disclosure_((title), (open), (EmProps){__VA_ARGS__}), em_disclosure_end_())
void em_disclosure_(const char *title, bool *open, EmProps p);
void em_disclosure_end_(void);

/* StatCard: a dashboard tile -- label, big value, signed delta, mini sparkline
 * (vals may be NULL). EmptyState: centred icon + title + subtitle block.
 * DividerLabel: a line--LABEL--line separator. */
void em_stat_card(const char *label, const char *value, const char *delta,
                  const float *vals, int n);
#define StatCard(label, value, delta, vals, n)  em_stat_card((label),(value),(delta),(vals),(n))
void em_empty_state(int icon, const char *title, const char *subtitle);
#define EmptyState(icon, title, subtitle)  em_empty_state((icon),(title),(subtitle))
void em_divider_label(const char *label);
#define DividerLabel(label)  em_divider_label(label)

/* --- navigation: tabs + split view -------------------------------------- */
typedef void (*EmPage)(void);   /* a page/tab is just a view function */
typedef struct { int icon; const char *label; EmPage page; } EmTab;
/* Renders items[*sel].page, then a bottom tab bar (icon+label, eased selection
 * pill). Compose with NavigationStack -- a tab's page may Push/Pop. */
void em_tabview(int *sel, const EmTab *items, int count);
#define TabView(sel, items, count)  em_tabview((sel), (items), (count))

/* SplitView: a fixed-width sidebar surface + a growing content pane. */
#define Split(sidew, ...)   EM_SCOPE_(em_split_((sidew), (EmProps){__VA_ARGS__}), em_split_end_())
#define SidebarPane(...)    EM_SCOPE_(em_sidebar_((EmProps){__VA_ARGS__}), em_end_())
#define ContentPane(...)    EM_SCOPE_(em_content_((EmProps){__VA_ARGS__}), em_end_())
void em_split_(float sidebar_w, EmProps p);
void em_split_end_(void);
void em_sidebar_(EmProps p);
void em_content_(EmProps p);

/* ======================================================================= */
/* EmUI V6 -- menus (menu bar, dropdown menus, context menus)               */
/* ======================================================================= *
 * A proper desktop menu system, built on the modal-overlay layer (so menus
 * float above content, and a click anywhere else closes them). One menu is
 * open at a time.
 *
 *   MenuBar {
 *       Menu("File") {
 *           if (MenuItem("New").shortcut("Ctrl+N").clicked()) new_doc();
 *           MenuSeparator();
 *           if (MenuItem("Quit").clicked()) quit();
 *       }
 *       Menu("Edit") { ... }
 *   }
 *
 *   // right-click anywhere -> a context menu at the pointer:
 *   static bool ctx; static float cx, cy;
 *   if (RightClicked(&cx, &cy)) ctx = true;
 *   ContextMenu(&ctx, cx, cy) { if (MenuItem("Copy").clicked()) copy(); }
 */

/* MenuBar: a horizontal strip of Menu buttons (put it at the top of a Window). */
#define MenuBar(...)  EM_SCOPE_(em_menubar_((EmProps){__VA_ARGS__}), em_menubar_end_())
void em_menubar_(EmProps p);
void em_menubar_end_(void);

/* Menu: a labeled button in the bar; its brace scope holds the MenuItems, shown
 * as a floating popover anchored below the button when open (click to toggle). */
#define Menu(label, ...)  EM_SCOPE_(em_menu_((label), (EmProps){__VA_ARGS__}), em_menu_end_())
void em_menu_(const char *label, EmProps p);
void em_menu_end_(void);

/* MenuItem: one row; returns true the frame it's clicked (which also closes the
 * open menu). MenuItemK adds a right-aligned shortcut hint ("Ctrl+S"). */
bool em_menu_item(const char *label, const char *shortcut);
#define MenuItem(label)       em_menu_item((label), 0)
#define MenuItemK(label, sc)  em_menu_item((label), (sc))
void em_menu_separator(void);
#define MenuSeparator()  em_menu_separator()

/* Right-click detection: returns 1 (once) on a right-press this frame, with the
 * press position written to out_x and out_y (window-content coords). Drive from
 * app's pointer state -- em_app_run does this automatically. */
int  em_right_clicked(float *out_x, float *out_y);
#define RightClicked(px, py)  em_right_clicked((px), (py))

/* ContextMenu: a popover anchored at (x,y) while *open; its scope holds
 * MenuItems. Clicking an item or outside closes it (clears *open). */
#define ContextMenu(open, x, y, ...) \
    EM_SCOPE_(em_context_menu_((open), (x), (y), (EmProps){__VA_ARGS__}), em_context_menu_end_())
void em_context_menu_(bool *open, float x, float y, EmProps p);
void em_context_menu_end_(void);

/* Runtime plumbing: the app loop feeds raw right-button state here (em_app_run
 * does it). A press EDGE of the right button becomes one em_right_clicked(). */
void em_feed_right_button(float x, float y, bool down);

/* ======================================================================= */
/* EmUI V7 -- multi-line text editing                                       */
/* ======================================================================= *
 * TextEditor: an editable multi-line text area. `buf`/`cap` own the text (a
 * NUL-terminated C string the app keeps), `cursor` is a byte offset into it the
 * app persists across frames, `height` is the visible height (it scrolls when
 * the text is taller). Handles typing, Enter (newline), Backspace, Delete, Tab,
 * and cursor movement via the arrow / Home / End keys (which the kernel
 * keyboard driver delivers as the EMBK_KEY_* private codes). Returns true while
 * focused. Click to focus.
 *
 *   static char  doc[4096] = "Hello\nEmbLink.";
 *   static int   cur = 0;
 *   TextEditor(doc, sizeof doc, &cur, 240);
 */
bool em_text_editor(char *buf, size_t cap, int *cursor, float height);
#define TextEditor(buf, cap, cursor, height) em_text_editor((buf), (cap), (cursor), (height))

/* Structural-change epoch: bumps when a V4/V6 component restructures the tree
 * (dropdown/menu open/close, toast raise/expiry, disclosure toggle, tab switch).
 * An app's loop compares it across frames and forces a full repaint on change. */
int em_ui_epoch(void);

/* ======================================================================= */
/* EmApplication -- the declarative app runtime (V4.1).                     */
/* One macro replaces the whole main(): font/resource setup, arenas, theme, */
/* window creation (kernel-chrome or chromeless), the mover binding, the    */
/* event loop with RETAINED updates (idle frames run NO ui work at all),    */
/* dirty-rect presents, and the app's own-close / ESC teardown.             */
/*                                                                          */
/*     EM_APPLICATION {                                                     */
/*         .title  = "V4 Demo",                                             */
/*         .size   = { 700, 560 },                                          */
/*         .theme  = Dark,                                                  */
/*         .chrome = Chromeless,                                            */
/*         .view   = app,                                                   */
/*     };                                                                   */
/* ======================================================================= */

typedef enum { ThemeAuto = 0, Light, Dark } EmTheme;
typedef enum { ChromeKernel = 0, Chromeless } EmChrome;
typedef enum { FixedSize = 0, Resizable } EmResize;
/* Window material. Acrylic = a frosted GLASS window: the compositor blurs the
 * desktop behind it and composites the window's translucent pixels over it, so
 * the title bar and gaps show the wallpaper frosted (implies Chromeless). */
typedef enum { MaterialSolid = 0, Acrylic } EmMaterial;

typedef struct {
    const char *title;
    struct { int w, h; } size;   /* content size; clamped to the screen */
    EmTheme     theme;
    EmChrome    chrome;          /* Chromeless -> the view draws Window/WindowBar */
    EmResize    resize;          /* Resizable -> Window() shows a corner grip;
                                  * dragging it resizes the OS window (V5) */
    EmMaterial  material;        /* Acrylic -> frosted glass window (V8) */
    void      (*view)(void);     /* the whole UI, rebuilt only when needed */
    const char *font;            /* resource path; default "/font.ttf" */
    int         pace_ms;         /* loop pace while active; default 10 */
} EmApp;

/* ---- desktop widgets (V5) ----------------------------------------------- *
 * A widget is a small always-on-desktop window: chromeless, z-banded ABOVE
 * the desktop but BELOW every app window. No keyboard, no close button --
 * it lives until its process is killed. `refresh_ms` re-runs the view on a
 * timer (a clock widget sets 1000), on top of the usual retained triggers.
 *
 *     EM_WIDGET { .title="Clock", .size={190,96}, .pos={24,24},
 *                 .refresh_ms=1000, .view=ClockView };                       */
typedef struct {
    const char *title;
    struct { int w, h; } size;
    struct { int x, y; } pos;
    EmTheme     theme;
    EmMaterial  material;        /* Acrylic -> frosted glass widget (V8) */
    void      (*view)(void);
    int         refresh_ms;      /* periodic rebuild; 0 = input/animation only */
    const char *font;
    int         pace_ms;
} EmWidget;

int em_widget_run(const EmWidget *wg);

#define EM_WIDGET \
    static EmWidget em_widget_spec_; \
    int main(void) { return em_widget_run(&em_widget_spec_); } \
    static EmWidget em_widget_spec_ =

/* Runs the app until its CloseButton/ESC quits. Defined in em_app.c, which is
 * only linked on-target (it speaks the EmbLink SDK); host tests never call it. */
int em_app_run(const EmApp *app);

#define EM_APPLICATION     static EmApp em_app_spec_;     int main(void) { return em_app_run(&em_app_spec_); }     static EmApp em_app_spec_ =

/* --- retained updates ---------------------------------------------------- *
 * The runtime SKIPS build+layout+render entirely on frames where nothing
 * changed (no pointer/key/wheel edge, no epoch bump, no requested frame).
 * Anything that animates asks for the next frame while it is live:
 * Spinner/Toast/em_animate/nav transitions do this automatically; an app
 * whose OWN state changes outside input (a clock, async data) calls
 * em_request_frame() itself when the state changes. */
void em_request_frame(void);
int  em_take_frame_request(void);   /* runtime-internal: read-and-clear */

/* --- resources ----------------------------------------------------------- *
 * Path-keyed caches for fonts and images, IO-agnostic: the host installs an
 * fopen-based loader, the target an embk-based one (em_app_run does this
 * automatically). em_font loads+registers a TTF once (and installs the text
 * backend on first success); em_image decodes a P6 .ppm once into BGRA-premul.
 * Image() shows a cached image scaled to .height. em_theme_use switches the
 * palette (Auto currently = Dark). */
void em_res_set_loader(uint8_t *(*load)(const char *path, size_t *out_len));
uint32_t em_font(const char *path);
const uint32_t *em_image(const char *path, uint32_t *out_w, uint32_t *out_h);
void em_image_view(const char *path, EmProps p);
#define Image(path, ...)  em_image_view((path), (EmProps){__VA_ARGS__})
void em_theme_use(EmTheme t);

/* frame + interaction plumbing */
void em_new_frame(void);              /* reset pending + clear id map, tick the clock (once per frame) */
void em_flush(void);                  /* emit the pending element now */
bool Clicked(const char *id);         /* did the element with this id fire this frame? */
bool Hovered(const char *id);

/* --- animation ---
 * Plug in a millisecond clock (embk_uptime_ms on target), then animate values.
 * em_animate eases a per-id retained scalar toward `target` at `rate` (~ how
 * fast; 8-14 feels good) and returns the current value -- drive a ProgressBar,
 * a colour, an offset, anything. Frame-rate independent. */
void em_set_clock(uint64_t (*now_ms)(void));
uint64_t em_now_ms(void);   /* current time (0 if no clock set) */
int   em_dt_ms(void);                 /* ms since the previous frame */
float em_animate(const char *id, float target, float rate);
#define Animate(id, target, rate) em_animate((id), (target), (rate))

/* Reusable views are just C functions. This is optional sugar for the no-arg
 * case so `Component(ProfileCard) { ... }` reads nicely; call it as ProfileCard(). */
#define Component(name) void name(void)

/* ------------------------------------------------------------------------- */
/* navigation -- a page stack of view functions                              */
/*     void HomePage(void) { ... if (Button("Settings").clicked()) Push(Settings); }  */
/*     void Settings(void) { NavBar("Settings"){ if(IconButton(IconChevronL).clicked()) Pop(); } ... } */
/*     // at the top level, each frame:  NavigationStack(HomePage);            */
/* ------------------------------------------------------------------------- */

void em_nav(EmPage root);      /* render the current top page (seeds the stack with root) */
void em_push(EmPage page);     /* navigate forward */
void em_pop(void);             /* navigate back */
int  em_nav_depth(void);       /* how many pages are on the stack */
int  em_nav_transitioning(void); /* !=0 while a page fade is in flight (force a full repaint) */

/* ---- modal overlay (Sheet / Alert) ------------------------------------- *
 * Declare an Overlay LAST in the screen so it paints on top. It dims the page
 * behind and centres a Dialog. OverlayDismissed() is true when the bare scrim
 * (outside the dialog) was clicked. em_overlay_active() tells the host loop to
 * force a full repaint while a modal is up (a page/modal show-hide is a big
 * structural change the dirty-rect renderer won't fully erase). */
#define Overlay()    EM_SCOPE_(em_overlay_(), em_overlay_end_())
#define Dialog(...)  EM_SCOPE_(em_dialog_((EmProps){__VA_ARGS__}), em_dialog_end_())
#define OverlayDismissed()  em_overlay_dismissed()
void em_overlay_(void);
void em_overlay_end_(void);
int  em_overlay_dismissed(void);
int  em_overlay_active(void);
void em_dialog_(EmProps p);
void em_dialog_end_(void);
#define NavigationStack(root) em_nav(root)
#define Page(fn)              (fn)     /* a page is just its view function */
#define Push(page)           em_push(page)
#define Pop()                em_pop()

/* ------------------------------------------------------------------------- */
/* icon codepoints (rendered as font glyphs -- DejaVu Sans has these)         */
/* ------------------------------------------------------------------------- */

#define IconCheck     0x2713   /* checkmark */
#define IconClose     0x2715   /* multiplication x */
#define IconStar      0x2605   /* filled star */
#define IconHeart     0x2665   /* heart */
#define IconChevronR  0x203A   /* single right angle quote */
#define IconChevronL  0x2039
#define IconPlus      0x002B
#define IconMinus     0x2212   /* minus sign */
#define IconGear      0x2699   /* gear */
#define IconBell      0x1F514  /* bell (may fall back) */
#define IconInfo      0x2139   /* information source */
#define IconWarn      0x26A0   /* warning sign */
#define IconBolt      0x26A1   /* high voltage */
#define IconSearch    0x1F50D
#define IconUser      0x1F464
#define IconDot       0x2022   /* bullet */
#define IconArrowR    0x2192
#define IconChevronD  0x2304   /* down arrowhead (Dropdown/Disclosure) */
#define IconChevronU  0x2303
#define IconInbox     0x2617   /* (DejaVu-safe placeholder tray) */
#define IconMagnify   0x26B2   /* magnifier-ish (DejaVu-safe search glyph) */
#define IconFiles     0x25A4   /* lined square (DejaVu-safe files/list) */
#define IconGrid      0x25A6   /* squared grid (dashboard tab) */
#define IconList      0x2630   /* trigram / list (list tab) */
#define IconHome      0x2302   /* house */
#define IconClock     0x1F550
#define IconFolder    0x1F4C1
#define IconDoc       0x1F4C4
#define IconTrash     0x1F5D1
#define IconCloud     0x2601

#endif /* __EMBLINK_EM_UI_H__ */
