# Building UI Apps with EmUI

This is a start-to-finish guide to writing a graphical app for EmbLinkOS with
**EmUI**, the OS's own SwiftUI-flavored UI toolkit. By the end you'll have a
window app, a desktop widget, and know how to wire either one into the boot
image so it shows up as a tile on the home screen.

If you want to know *how EmUI works internally* (the layer stack, the
compositor, retained updates under the hood) instead of *how to use it*, see
[EMUI_INTERNALS.md](EMUI_INTERNALS.md). If you haven't built the toolchain
yet, do that first: [BUILD_SETUP.md](BUILD_SETUP.md).

## Mental model

EmUI apps are **immediate-mode declarative**: you write a `view()` function
that describes the whole UI as nested C blocks, and call it every frame (or,
with the app runtime, only when something actually changed — see
[Retained updates](#retained-updates-you-get-for-free)). There's no manual
tree building, no diffing to write yourself, no widget objects to keep alive.
State lives in plain C variables; EmUI reads them each time it runs your view.

```c
static bool g_dark_mode = true;

static void SettingsPage(void) {
    VStack(.spacing = 12, .padding = 20) {
        Text("Settings").title();
        Toggle("Dark mode", &g_dark_mode);
        if (Button("Save").primary().clicked()) save_settings();
    }
}
```

`VStack(...)  { ... }` is a real C block guarded by a for-loop macro — the
braces matter, and you must never `return`/`break`/`continue`/`goto` out of
one (it skips the matching close and corrupts the tree). Modifiers are C
[designated initializers](https://en.cppreference.com/w/c/language/struct_initialization)
into one `EmProps` struct: `.spacing = 12` is SwiftUI's named-argument
`VStack(spacing: 12)`. A zero-valued field means "use the theme default."

Chainable leaves like `Text(...)` and `Button(...)` return a small
vtable-of-function-pointers you can call methods on:
`Text("Hi").caption().secondary()`, `Button("Delete").destructive().id("del")`.

## Your first app: EM_APPLICATION

A window app is a `view` function plus one declaration that **replaces
`main()` entirely**:

```c
/* user/bin/hello_ui.c */
#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

static void app(void) {
    Screen(.justify = Center, .align = Center) {
        Card(.width = 280, .spacing = 12) {
            Text("Hello, EmbLink!").title();
            Text("Your first EmUI app.").caption().secondary();
        }
    }
}

EM_APPLICATION {
    .title = "Hello UI",
    .size  = { 400, 300 },
    .theme = Dark,
    .view  = app,
};
```

That's the whole program. `EM_APPLICATION { ... }` expands to a `main()` that
calls `em_app_run(&spec)`, which does everything a GUI app needs:
- loads your font (`/font.ttf` by default) and installs the text backend,
- creates arenas, sets the theme, brings up the toolkit,
- opens the OS window (kernel-chrome by default — see
  [App-owned chrome](#app-owned-chrome-chromeless-windows) for the
  alternative),
- runs the event loop with [retained updates](#retained-updates-you-get-for-free),
- presents only the pixels that changed,
- tears down on `ESC` or your own close control.

`EmApp` fields (all optional except `.view`):

| Field | Meaning | Default |
|---|---|---|
| `.title` | window title (kernel chrome) / serial log prefix | `"EmApp"` |
| `.size = { w, h }` | requested content size; clamped to the screen | 640×480 |
| `.theme` | `Light` / `Dark` / `ThemeAuto` | `ThemeAuto` (= Dark) |
| `.chrome` | `ChromeKernel` (default) or `Chromeless` | `ChromeKernel` |
| `.resize` | `FixedSize` (default) or `Resizable` | `FixedSize` |
| `.view` | your top-level view function | **required** |
| `.font` | resource path for the app font | `"/font.ttf"` |
| `.pace_ms` | event-loop sleep between polls | `10` |

## Containers, modifiers, and layout

EmUI's layout engine is a flexbox subset (SwiftUI's model, not a full CSS
solver): a stack is a row or column, children lay out along the main axis,
`.align`/`.justify` control the cross/main axis, `.spacing` is the gap
between children, `.grow` on a leaf makes it fill remaining space.

```c
VStack(.spacing = 16, .padding = 20, .align = Fill) {
    HStack(.spacing = 8) {
        Text("Left");
        Spacer();                 // pushes the next item to the far end
        Text("Right");
    }
    Divider();
    HStack { Text("Grows:"); TextField(buf, sizeof buf, "type here").grow(); }
}
```

Available containers: `VStack`, `HStack`, `ZStack`, `Card` (surface + border +
shadow + padding preset), `Screen` (full-size root), `Section(title)` (a
labeled group), `Row`, `NavBar(title)`, `ScrollView(&scroll_offset, height)`,
`List` (grouped inset surface) with `ListRow(icon, title, value)` rows.

`EmAlign` values: `Leading`, `Center`, `Trailing`, `Fill`, `SpaceBetween`.
`EmStyle` (buttons): `Primary`, `Secondary`, `Ghost`, `Destructive`.
`EmTone` (badges/tags/banners/toasts): `Accent`, `Success`, `Warning`,
`Danger`, `Neutral`.

## Text, controls, and state binding

Controls bind directly to your variables — no getters/setters, no observers:

```c
static bool  g_wifi = true;
static float g_volume = 0.6f;
static int   g_devices = 3;
static char  g_name[32] = "Ada";

Toggle("Wi-Fi", &g_wifi);
Slider(&g_volume);
Stepper("Devices", &g_devices, /*lo=*/1, /*hi=*/10);
TextField(g_name, sizeof g_name, "Your name");
```

Text has five font roles (`.title() .heading() .body() .bold() .caption()`)
and color roles (`.secondary() .tertiary() .accent()`). A leaf you want to
read a click from ends with `.clicked()`; give it `.id("x")` first if you
need to check it again later with `Clicked("x")`.

```c
if (Button("Delete").destructive().clicked()) delete_item();
IconButton(IconGear).id("settings");
...
if (Clicked("settings")) open_settings();   // checked elsewhere in the tree
```

Icons are Unicode codepoints from a DejaVu-Sans-safe set —
`IconCheck`, `IconClose`, `IconStar`, `IconGear`, `IconSearch`, `IconBolt`,
`IconChevronR/L/U/D`, `IconGrid`, `IconFiles`, `IconFolder`, `IconTrash`,
`IconCloud`, and more — see the bottom of `ui/dsl/em.h` for the full list.

## Component reference

| Component | Signature | Notes |
|---|---|---|
| `Badge` / `Tag` | `(text)` / `(text).tone(...)` | small status pills |
| `Avatar` | `(initials)` | circular initials |
| `Banner` | `(icon, text).tone(...)` | inline alert row |
| `ProgressBar` | `(0.0–1.0)` | linear progress |
| `Chart` | `(vals, n, .height=...)` | mini bar chart |
| `LineChart` / `AreaChart` | `(vals, n, .height=...)` | rasterized polyline (±fill) |
| `Spinner` | `()` | indeterminate activity, animates itself |
| `Gauge` | `(frac, center_text, .height=...)` | rasterized ring, cached |
| `Dropdown` | `(labels[], count, &sel)` | inline expanding menu |
| `SearchField` | `(buf, cap, placeholder)` | text field + search/clear icons |
| `Disclosure` | `(title, &open) { children }` | expandable section |
| `StatCard` | `(label, value, delta, vals, n)` | dashboard tile with sparkline |
| `EmptyState` | `(icon, title, subtitle)` | centered placeholder |
| `DividerLabel` | `(text)` | labeled section divider |
| `Image` | `(path, .height=...)` | see [Resources](#resources-fonts--images) |
| `MenuBar` / `Menu` | `MenuBar(){ Menu("File"){ items } }` | drop-down menu strip, see [Menus](#menus-v6) |
| `ContextMenu` | `ContextMenu(&open, x, y){ items }` | right-click popover |
| `TextEditor` | `(buf, cap, &cursor, height)` | multi-line editor, see [Text editor](#text-editor-v7) |

## Navigation

**Push/pop stack** — a page is just a `void fn(void)`:

```c
static void SettingsPage(void) {
    NavBar("Settings") { if (IconButton(IconChevronL).clicked()) Pop(); }
    ...
}
static void HomePage(void) {
    if (Button("Settings").clicked()) Push(SettingsPage);
}
// at the root, every frame:
NavigationStack(HomePage);
```

**Tabs** — `TabView(&sel, EmTab items[], count)` where each `EmTab` is
`{ icon, "label", PageFn }`; renders a bottom tab bar with an eased selection
pill and the current page above it.

**Split view** — `Split(sidebar_width) { SidebarPane(){...} ContentPane(){...} }`
for a desktop-style two-pane layout.

## Menus (V6)

A **menu bar** is a strip of drop-down `Menu`s; a **context menu** is the same
popover opened by a right-click. Both float above content in the overlay layer
and dismiss on an outside click.

```c
MenuBar() {
    Menu("File") {
        if (MenuItemK("New",  "Ctrl+N")) do_new();
        if (MenuItemK("Open", "Ctrl+O")) do_open();
        MenuSeparator();
        if (MenuItemK("Quit", "Ctrl+Q")) quit();
    }
    Menu("Edit") { if (MenuItem("Undo")) undo(); /* ... */ }
}
```

`MenuItem(label)` and `MenuItemK(label, shortcut)` each **return `bool`** — true
on the frame they're chosen (act on it directly in an `if`). `MenuSeparator()`
draws a divider. Clicking a bar `Menu` toggles its panel open at the label; an
item click or an outside click closes it.

**Context menu** — anchor a popover where the pointer was right-pressed:

```c
static bool ctx = false; static float cx, cy;
if (RightClicked(&cx, &cy)) ctx = true;      // right-press writes the anchor
ContextMenu(&ctx, cx, cy) {
    if (MenuItem("Copy"))  copy();
    if (MenuItem("Paste")) paste();
}
```

The `EM_APPLICATION` runtime feeds the right mouse button in for you, so
`RightClicked()` fires anywhere in the content. `v6demo.c` (the "Menus" home
tile) is the reference app.

## Text editor (V7)

`TextEditor(buf, cap, &cursor, height)` is a focusable, scrolling, multi-line
plain-text editor over a caller-owned `char buf[cap]` with a byte `cursor`.
Click it to focus; then it takes typed characters, `Enter` (newline), `Tab`
(two spaces), `Backspace`/`Delete`, and the `Left`/`Right`/`Up`/`Down`/`Home`/
`End` navigation keys. The view auto-scrolls to keep the caret visible, and it
returns `true` while focused.

```c
static char doc[4096] = "Type here...\n";
static int  cur = 0;
VStack(.spacing = 10, .padding = 16, .align = Fill) {
    Text("Notes").heading();
    TextEditor(doc, sizeof doc, &cur, 320);   // 320px tall
}
```

The arrow/Home/End keys arrive as `EMBK_KEY_*` codes the kernel keyboard driver
emits for the extended scancodes; under `EM_APPLICATION` the runtime grabs the
keyboard and forwards everything, so no extra wiring is needed. `v7demo.c` (the
"Editor" home tile) is the reference app.

## Materials: glass

EmbLink's signature material is **glass** — a frosted panel that blurs whatever
is behind it, tinted with the theme surface nudged toward the EmbLink accent,
and finished with a light edge highlight for depth. Use it for chrome, menus,
and sheets to make the UI feel layered rather than flat.

Two equivalent ways to get it:

```c
Glass(.width = 360, .padding = 20, .spacing = 12) {   // dedicated container
    Text("Panel").title();
    Text("I blur what's behind me.").caption().secondary();
}

VStack(.glass = 1, .corner = 16, .padding = 20) { ... }  // any box, .glass = 1
```

`.blur = <radius>` overrides the default blur (12px). Glass sets its own fill
and border, so don't also set `.background`; you can still add `.corner`,
`.padding`, `.shadow`, and sizing. The tint is translucent (it rides the fast
integer blend path), but the **blur itself is the one expensive primitive** on
this CPU renderer — use glass on panels that sit still (menus, bars, dialogs),
not on every card, and not over fast-animating content. Because apps run a
retained loop, a static glass panel over a static backdrop blurs **once** and
then costs nothing until something behind it changes. The V6 menus are glass
by default; `make showcase-v2` with the `g` gallery renders a glass panel over
a color field (`build/glass_{dark,light}.png`).

**Frosted windows (Acrylic).** A whole window can be glass — the compositor
blurs the *desktop* behind it and composites the window over it at ~85%, so the
title bar and the gaps around your content show the wallpaper and other windows
frosted through. Opt in from the app runtime:

```c
EM_APPLICATION {
    .title    = "Editor",
    .chrome   = Chromeless,
    .material = Acrylic,     // frosted window (implies Chromeless)
    .view     = app,
};
```

That's the whole change — your view still renders normally (opaque); the
compositor does the frosting (`EMBK_WINF_GLASS`, a kernel-side blur + uniform
alpha). The blur runs when the window paints, so a still window is cheap; a
*drag* re-blurs each step (fine under a GPU, a little heavier under plain QEMU).
`v7demo.c` (the "Editor" tile) is an Acrylic window.

## App-owned chrome: chromeless windows

By default `EM_APPLICATION` gets a **kernel-drawn** title bar and close
button. Set `.chrome = Chromeless` and the kernel draws *nothing* — your view
must supply `Window`/`WindowBar`, and you get to design the close control
yourself:

```c
static void app(void) {
    Window("My App") {
        WindowBar("My App") {
            if (CloseButton().clicked()) { /* your own close logic */ }
        }
        ... your content ...
    }
}

EM_APPLICATION {
    .title = "My App",
    .size = { 640, 480 },
    .chrome = Chromeless,
    .view = app,
};
```

`WindowBar`'s interior is a drag zone: press and move anywhere on it (except
on a control you place inside it) and the window follows the pointer — the
runtime wires this up automatically. `CloseButton()` is one round control
that tints red on hover; put whatever you want next to it (a menu, more
icons) — the bar is yours. `em_window_closed()` is what the runtime checks to
know your own close control fired; you don't call it yourself unless you're
hand-rolling the loop (see [`user/bin/home.c`](../user/bin/home.c) for that
lower-level pattern).

Add `.resize = Resizable` and `Window()` draws a small corner grip; dragging
it and releasing resizes the OS window and the whole app re-lays-out at the
new size.

## Desktop widgets

A **widget** is a small window that lives on the desktop permanently: no
title bar, no close button, and it sits in its own z-band — always above the
wallpaper, always below every app window, so apps naturally float over it.
Use `EM_WIDGET` instead of `EM_APPLICATION`:

```c
/* user/bin/clockw.c (shipped as the reference widget) */
#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

static void ClockView(void) {
    uint64_t secs = embk_uptime_ms() / 1000;
    static char t1[16];
    snprintf(t1, sizeof t1, "%lu:%02lu", secs / 60, secs % 60);
    Window("Clock") {
        VStack(.padding = 12) { Text(t1).title(); }
    }
}

EM_WIDGET {
    .title = "Clock", .size = { 190, 108 }, .pos = { 24, 24 },
    .theme = Dark, .refresh_ms = 1000, .view = ClockView,
};
```

`.refresh_ms` re-runs the view on a timer — set it to 1000 for a clock, leave
it 0 for a widget that only updates on input/animation. A widget has no
keyboard and no close button; it lives until its process is killed. Home
spawns `clockw.elf` automatically at boot as a live example.

## Resources: fonts & images

`em_font(path)` and `em_image(path)` are path-keyed caches: call them as
often as you like, the file is only loaded/decoded once. `EM_APPLICATION`/
`EM_WIDGET` already call `em_font()` for you (`.font`, default `/font.ttf`);
you only need these directly for extra fonts or images:

```c
static uint32_t g_icon_font;
g_icon_font = em_font("/icons.ttf");   // cached after the first call

Image("/logo.ppm", .height = 64);      // P6 PPM only; missing file -> a quiet placeholder box
```

The loader itself is injected (`em_res_set_loader`) — the app runtime installs
the on-target `embk_open`/`embk_read` loader automatically, which is why
resource paths are OS filesystem paths (they must exist in EMBKFS, see
[Wiring your app into the boot image](#wiring-your-app-into-the-boot-image)).

## Retained updates (you get for free)

`em_app_run`/`em_widget_run` don't rebuild the UI every loop iteration. A
frame is only built when there's an actual reason to: a pointer/key/wheel
edge, a structural change (dropdown opened, tab switched, toast raised), a
`.refresh_ms` tick, or a component that's mid-animation asking for the next
frame. `Spinner`, `Toast`, `em_animate`, and page transitions call
`em_request_frame()` for you automatically. If your *own* state changes
outside of user input — a background counter, data arriving asynchronously —
call `em_request_frame()` yourself right after you change it, or the runtime
won't know to redraw. An idle app costs one input poll and a sleep; nothing
else runs.

## Wiring your app into the boot image

**Just drop the `.c` file in `user/bin/` and rebuild — that's it.** The build
system auto-discovers apps: any `user/bin/*.c` (except the two special-linked
programs `init.c` and `hello.c`) is treated as a dynamically-linked EmUI app,
built by a Makefile pattern rule and packed onto the boot image by
`mkfs_embkfs.py`'s `discover_userland_objects()` (which globs `build/*.elf`).
No per-app Makefile rule, no mkfs edit.

```
# create user/bin/hello_ui.c with a view + EM_APPLICATION, then:
make embkfs.img            # discovers, builds, and packs hello_ui.elf automatically
make run-embkfs-cow        # boots to the desktop; see CONTRIBUTING.md for run targets
```

The **only** optional manual step is a home-screen tile: in
[`user/bin/home.c`](../user/bin/home.c)'s tile grid, add
`tile("Hello UI", "/hello_ui.elf");` if you want to launch it by click rather
than by `run /hello_ui.elf` at the shell.

Every EmUI app links **dynamically** against the shared toolkit
(`libembk.so`), which is why apps stay small (~370–380 KB each vs. bundling
the whole toolkit) — see
[BUILD_SETUP.md](BUILD_SETUP.md#dynamic-linking-how-libembkso-actually-works)
for what happens at load time.

> One build-hygiene note: the packer globs `build/*.elf`, so if you *delete*
> an app's `.c`, its stale `build/<name>.elf` will keep getting packed until
> you `make clean` (or `rm build/<name>.elf`). Adding apps needs nothing;
> only removing one needs that cleanup.

## Testing without booting a whole OS

The lower toolkit layers compile and run **on your host** — no QEMU needed —
which is the fast loop for anything that isn't window-management-specific:

```
make scene-test backend-test font-test layout-test reactive-test declare-test   # unit tests
make showcase-v2       # renders EmUI DSL screens to build/v2_{light,dark}.png
                        # and V4/V5 galleries to build/v4_{light,dark}.png
```
`ui/dsl/showcase_v2.c` is a good reference for exercising a new component
against both themes before ever booting a VM. Once the pixels look right,
verify interaction live: `make run-ui` (boots to a shell; type
`run /uidemo.elf`) or `make run-embkfs-tree` / `run-embkfs-cow` to boot
straight to the home launcher and click your tile.

## Common pitfalls

- **Never `return`/`break`/`goto` out of a container block.** The brace-scope
  macros rely on the for-loop running to completion to close the scope;
  jumping out skips `em_end_()` and desyncs the whole tree for the rest of
  the frame.
- **A leaf is "pending" until the next boundary.** `Text("x").caption()`
  builds up a modifier chain and only actually emits into the tree when the
  next leaf/container starts or the frame ends (`em_flush()`). This is
  invisible in normal use but matters if you're calling the lower-level
  `em_*_impl` functions directly.
- **State lives outside the view function.** `static` locals or file-scope
  globals — the view function itself is called fresh every build, so a plain
  local variable resets every time.
- **`Image()` needs a real EMBKFS path**, not a host path — package the
  asset the same way `font.ttf` is packaged (add it to `mkfs_embkfs.py`'s
  object list).
- **Forgetting `em_request_frame()`** for your own async state is the most
  common "why isn't my UI updating" bug once you're past the tutorial stage
  — see [Retained updates](#retained-updates-you-get-for-free).

## Where to go next

- [EMUI_INTERNALS.md](EMUI_INTERNALS.md) — how the layers underneath this
  guide actually work, for anyone extending the toolkit itself.
- [`user/bin/v4demo.c`](../user/bin/v4demo.c) — the fullest reference app:
  chromeless chrome, tabs, split view, every V4/V5 component in one place.
- [`user/bin/clockw.c`](../user/bin/clockw.c) — the minimal widget reference.
- [`ui/dsl/showcase_v2.c`](../ui/dsl/showcase_v2.c) — every component
  exercised for the host-render test loop.
