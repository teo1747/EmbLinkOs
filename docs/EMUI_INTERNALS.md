# EmUI Internals

This is the architecture reference for people **extending EmUI itself** — new
primitives, new components, compositor changes, performance work. If you just
want to build an app, read [EMUI_GUIDE.md](EMUI_GUIDE.md) instead; nothing
here is required to use the toolkit.

## The layer stack

EmUI is nine small pieces, each doing one job, stacked bottom-to-top. Nothing
above a layer reaches back down past its immediate dependency — the DSL
(`em.h`) never touches `scene.h` directly, `ui.h` never touches the render
backend directly, and so on. This is what lets each piece have its own
host-run unit test (`make scene-test`, `layout-test`, ... — no QEMU needed)
and what makes the dependency direction easy to keep straight when adding
something new.

```
ui/dsl/em.{h,c}, em_app.c      SwiftUI-flavored DSL + EM_APPLICATION/EM_WIDGET runtime
        |
ui/kit/kit.{h,c}               themed widget kit (buttons, toggles, lists, ...)
        |
ui/theme/theme.{h,c}           design tokens (colors, spacing, type scale)
        |
ui/declare/                    immediate-mode declarative API + the retained instance tree
   ui.h / instance.h / declare.c
        |              \
ui/layout/          ui/reactive/
layout.{h,c}         scope.h, signal.h, reactive.c
   flexbox engine       general reactive primitive (signals + effect scopes)
        |
ui/scene/scene.{h,c}           the resolved render IR: a scene graph, nothing else
        |
ui/backend/                    scene -> pixels
   backend.h (vtable) / cpu_backend.c / font.{h,c} / scene_render.{h,c} / render_target.h
```

Two more pieces sit outside this stack because they're not toolkit code:
**`kernel/gfx/compositor.c`** (in-kernel: windows, z-order, chrome, the
pointer) and **`user/lib/embk.h`** (the syscall surface every app and the
runtime call into to talk to the compositor).

### `ui/scene` — the render IR

The lowest layer and the one everything else is built on. A `scene_arena` is
a **paged, generation-counted arena**: nodes live in fixed-size pages
(`SCENE_PAGE_SIZE = 256`), allocated lazily as an array of `NULL`-until-used
pointers (`struct scene_arena.pages[SCENE_MAX_PAGES]`), so up to 65536 nodes
cost only unused pointer slots until you actually create them. A
`struct node_handle { index, generation }` is how everything above this layer
refers to a node — the generation is bumped on `scene_destroy_node`, so a
stale handle held past a node's death resolves to `NULL` instead of
aliasing a reused slot (this is what invariant "N2" refers to in the source
comments). This ABA-safe-handle pattern repeats at every layer above
(`layout_handle`, `instance_handle`) — if you add a new arena-backed
structure, follow the same shape.

A node is one of four kinds (`SCENE_NODE_GROUP/RECT/IMAGE/TEXT`) with a TRS
transform, size, paint (`solid` or a linear gradient), optional shadow/
border/opacity/clip, and — for GROUPs — a first-child/next-sibling tree.
`scene_set_*` functions are the only way to mutate a node; most of them are
**no-op-guarded**: they compare the new value against what's already stored
and skip the write (and the resulting repaint) if nothing actually changed.
This guard is why `scene_mark_dirty()` exists as an escape hatch — if a
caller mutates a buffer a scene node points to *in place* (rather than
handing in a new pointer), the guard can't see the change and you must force
it explicitly. (This bit a real bug once: `declare.c`'s `set_text_on` copies
new text into a durable shadow buffer, then calls `scene_set_text` with that
*same* pointer — old and new alias the same storage, so the byte-compare
guard could never see a difference. Fixed by calling `scene_mark_dirty()`
after the copy, since the caller already proved the content changed. Keep
this pattern in mind for any new setter with in-place-mutated storage.)

`scene_traverse()` walks the tree depth-first in paint order and is the only
thing that needs to understand parent/child relationships — the render
backend below doesn't walk the tree itself, it's *driven* by traversal
callbacks.

### `ui/layout` — the flexbox engine

A **separate** paged arena, one-to-one paired with scene nodes (a
`layout_node` carries the `node_handle` of the scene node it positions). This
split exists because layout state (axis, justify, align, flex weights) has
nothing to do with the render IR, and because layout needs a two-phase
algorithm scene nodes have no business knowing about:

1. **Intrinsic pass** (bottom-up): every node measures its natural size —
   text measures at its font's natural width via `font.h`, fixed-size nodes
   report their fixed size, containers sum their children.
2. **Arrange pass** (top-down): starting from the root with a known W×H,
   `layout_run` distributes space along the main axis by `SIZE_FIXED` /
   `SIZE_INTRINSIC` / `SIZE_FLEX` weight, resolves cross-axis alignment
   (`ALIGN_START/CENTER/END/STRETCH`), and writes final `resolved_x/y/w/h`
   into each layout node, then pushes that geometry onto the paired scene
   node's transform.

**CSS semantics that are easy to get backwards, and were a real bug fix**:
`ALIGN_STRETCH` only overrides a child's cross size when that child's size
mode is `SIZE_INTRINSIC` (auto). A child with an explicit `SIZE_FIXED` cross
size is *never* stretched — order the checks fixed-size-wins-first if you
touch this code (see `layout_test.c`'s T4 for the regression test, which
originally only checked the stretch case and had to be extended to also
assert the fixed-size case once the two started interacting). Separately,
the **root layout node defaults to a stretch column** (`ui_init` sets
`ln->axis = AXIS_COLUMN; ln->align = ALIGN_STRETCH`) — without this, a
full-size `Screen`/`Window` root that should fill the OS window instead
top-packs at its intrinsic height, which silently breaks anything anchored
to the bottom (a tab bar, a resize grip) until the window happens to be
exactly that tall.

### `ui/reactive` — signals and scopes

A general-purpose reactive primitive, intentionally UI-agnostic (no
dependency on `scene.h` or `layout.h`). Two pieces:

- **`signal.h`**: `signal_create(initial)` / `signal_get` (reads the value
  and, if called while a scope is actively tracking, records a dependency
  edge) / `signal_set` (skips the write and dependent-marking entirely if the
  new value byte-compares equal — the same no-op-guard philosophy as
  `scene_set_*`).
- **`scope.h`**: `scope_create(fn, ctx)` is **the one primitive** for both a
  component body and a plain effect — there's no separate "effect" type.
  `scope_rerun` clears the scope's old dependency edges, re-runs it (which
  re-tracks new edges as `signal_get` calls happen inside), and
  `reactivity_flush()` re-runs every scope a `signal_set` marked dirty,
  batched to a frame boundary rather than firing synchronously on every set.
  Edges are stored bidirectionally so removing a scope is O(edge count), not
  O(all signals).

In practice, most EmUI app code today doesn't reach for signals directly —
`EM_APPLICATION`'s retained-update model (see below) plus plain C statics
covers the common case, and `ui_component()` in `ui.h` is the hook if you
want a reactive subtree. The reactive layer exists underneath both paths.

### `ui/declare` — the retained instance tree + the immediate-mode API

This is the layer that turns "call a C function every frame" into "a stable
tree that only changes where the app's declarations actually changed" —
`instance.h`'s comment puts it exactly: a `Button` and a `VStack` are both an
`INSTANCE_BOX` with different property presets; the four kinds are
`INSTANCE_TEXT / INSTANCE_BOX / INSTANCE_IMAGE / INSTANCE_COMPONENT`. Each
`instance` owns exactly one scene node + one layout node (and, for
`INSTANCE_COMPONENT`, a reactive `scope`), plus a `ui_shadow` recording the
last-declared property values so repeated `ui_set_*` calls with unchanged
values are free.

**Reconciliation** (`match_or_create` in `declare.c`) is how the "immediate
mode that behaves like retained mode" trick works, and it's worth
understanding exactly because it's the thing every container/leaf macro
ultimately calls into:

- **Positional matching (the default, no explicit key)**: a child is matched
  against whichever sibling comes next in traversal order, IF that sibling
  is the same `instance_kind` and hasn't already been visited this frame.
  This is why a static list of widgets Just Works without keys — same
  shape, same order, every frame, so position IS identity.
- **Keyed matching** (`.id("x")`, or `ui_box_begin(key)`/`ui_button_keyed`):
  scans the parent's children for one with a matching `explicit_key`,
  regardless of position — this is what you need the moment children can
  reorder, get inserted in the middle, or get removed (a dynamic list, a
  filtered view). If a keyed slot's kind changed since last frame, the old
  instance is destroyed and a fresh one created under the same key rather
  than reused.
- Every `ui_*_begin` call does `reset_children_unvisited(parent)` first and
  every `ui_*_end` does `sweep_unvisited_children(parent)` last — any child
  instance that *wasn't* re-declared (and thus never got `visited_this_run`
  set) this frame is destroyed. This is the GC: a component that stops
  rendering a subtree gets that subtree's instances (and their scene/layout
  nodes) cleaned up automatically, no manual teardown required.

**Rule for anyone adding a new component that renders a dynamic-length list**:
give each row an explicit key derived from stable data (an id, not an
index) — an index-keyed list breaks identity across reorders/removals in
exactly the way `ui_shadow` diffing is supposed to prevent.

`ui.h` is the immediate-mode surface everything above (kit, em) is built on:
`ui_box_begin/end`, `ui_begin_vstack/hstack`, the `ui_set_*` property
setters, leaves (`ui_text`, `ui_button`, `ui_image`), input dispatch
(`ui_pointer`, `ui_dispatch_click`, `ui_is_hovered/pressed/active`,
`ui_consume_click`), and the frame lifecycle (`ui_init`, `ui_frame_begin/end`,
`ui_run_layout`). If you're adding a genuinely new *kind* of interactive
primitive (not just a new themed preset over an existing one), this is the
layer you extend — `kit.h` and `em.h` are both just curated call patterns
over `ui.h`.

### `ui/theme` — design tokens

One `struct ui_theme`: surfaces, a three-step text hierarchy
(`text`/`text_secondary`/`text_tertiary`), a single accent color plus
semantic status colors (`success`/`warning`/`danger`, deliberately separate
from the accent), an 8px spacing scale (`sp1`..`sp7` = 4/8/12/16/24/32/48),
a five-step type scale, three shadow presets, and the two active font
handles. `ui_theme()` returns the active theme (never `NULL`);
`ui_theme_use_dark(bool)` swaps the whole palette in one call. `kit.h` reads
*exclusively* from these tokens — the rule for both `kit` and `em` component
code is: no raw colors, no raw pixel constants, everything traces back to a
token so a full re-theme is one function call, not a grep-and-replace.

### `ui/backend` — scene to pixels

`backend.h` defines a small vtable (`draw_rect`, `draw_image`,
`draw_shadow`, `draw_border`, `draw_backdrop_blur`, `draw_text`,
`push_clip`/`pop_clip`, `begin_frame`/`end_frame`); `cpu_backend.c` is the
one implementation (`cpu_backend_get()`). `scene_render.c` is the driver: it
calls `scene_traverse` and dispatches each visited node to the right vtable
call, tracking a **dirty-rect cache** of last frame's world-space rects per
node so `scene_render_frame` can report back exactly which screen regions
actually changed (`r.dirty[]`, `r.n_dirty`, `r.full`) instead of the caller
always doing a full repaint. `font.c` is a from-scratch TrueType rasterizer
(glyf outlines only — no CFF, no hinting, no kerning) with a glyph atlas
cache; `font_install_backend()` is what actually plugs `draw_text` into the
CPU backend (it's `NULL` until called, which is why every app calls it right
after loading its font).

**The one hard performance lesson from this layer, worth repeating for
anyone adding a new drawing primitive**: under this project's QEMU/TCG
environment (no KVM acceleration available), **per-pixel floating-point work
is poison; per-pixel integer work is fine**. A naive SDF-based rounded-rect
fill or a float-math ring gauge that re-rasterizes every frame can single-
handedly cost hundreds of milliseconds per frame. The fixes that mattered in
practice were: (1) a fast path for fully-opaque interior pixels that skips
SDF/coverage math entirely and does a bare integer store, (2) computing
borders by walking only the actual border band instead of the whole bounding
box, (3) integer reciprocal-multiply instead of a real divide in the blur
kernel, (4) **caching** any rasterized-once-per-value bitmap (the `Gauge`
ring is keyed on a quantized fraction + color and only re-rasterizes when
that key changes) rather than redrawing it every frame, (5) a
**constant-alpha integer-LUT** fast path for translucent solid fills (the
modal scrim and tinted panels: build four 256-entry `dst→out` blend tables
once per rect, then each interior pixel is a table lookup, not a float
`blend_over`), (6) **table-based gamma text** — the glyph blend mixes in
linear light (`out = sqrt(src²·eff + dst²·inv)`), which done literally was 3
per-pixel `sqrtf`s, and in the freestanding `-mno-sse` build `sqrtf` is an
8-iteration Newton loop (~24 float divides per text pixel). `font.c` replaces
it with two static tables — `g_lin[d]` linearizes a destination byte and
`g_delin[k]` de-linearizes via a 12-bit sqrt table — measured ~5× faster on
the text path with no visible change, and (7) an **integer premultiplied
source-over** fast path in `draw_image` (opaque source = a bare copy;
translucent = the same carry-safe `dst*(255-a)/255 + src` trick the shadow
blit uses) for full-opacity, full-coverage image pixels — this is what the
`Gauge` ring, sparklines, and `LineChart`/`AreaChart` composite through. If
you're adding a new rasterized primitive, budget for this from the start
rather than optimizing after the fact: keep the *inner* per-pixel loop
integer, hoist any float or transcendental work out to per-primitive setup or
a precomputed table.

**The glass material.** `draw_backdrop_blur` (sample the region behind a box,
box-blur it, write it back SDF-clipped to the rounded shape) is what makes
frosted glass possible; it's exposed as `ui_set_backdrop_blur(enabled, radius)`
and, at the DSL top, as `.glass = 1` / the `Glass()` container (see
`em_glass_apply` in `em.c`): backdrop blur + a translucent theme-surface tint
nudged toward the accent + a light edge-highlight border. The tint is `< 1.0`
alpha so it rides the constant-alpha integer-LUT path (cheap); the **blur is
the only costly primitive**, so glass is meant for panels that hold still
(menus, bars, sheets) — and because the render is dirty-rect + retained, a
static glass panel over a static backdrop blurs once and then idles. A menu
panel's backdrop blur samples the render target *after* the content behind it
is drawn (menus live in the overlay layer, painted last), so it correctly
frosts whatever it covers.

**Frosted windows (Acrylic) are done in the compositor**, not the toolkit,
because a window needs to frost the *desktop* behind it — pixels the app can't
see. `paint_region` already paints the desktop and lower windows before it
blits each window on top, so the backdrop is sitting in the framebuffer at that
moment. A window created with `EMBK_WINF_GLASS` (`comp_window.glass`, set via
`.material = Acrylic` on `EM_APPLICATION`) takes a different blit path in
`paint_window`: `fb_blur_region` box-blurs the framebuffer strip the window is
about to cover, then `fb_blit_uniform` lays the window's (opaque) pixels over it
at a fixed ~85% alpha — so the whole window, title bar included, reads as
frosted acrylic with no app-side render changes. Cost is bounded to the window's
damage rect, and paid only when the window paints; a *drag* re-blurs each step
(cheap on real GPUs, heavier under plain QEMU/TCG). See
`kernel/drivers/video/framebuffer.c` (`fb_blit_uniform`, `fb_blur_region`) and
`kernel/gfx/compositor.c` (`GLASS_BLUR`, `GLASS_ALPHA`).

### `ui/dsl/em.{h,c}` — the DSL

A thin macro + function layer with three ideas, all defined in `em.h`/`em.c`:

1. **Brace-scoped containers** via the `EM_SCOPE_(open, close)` for-loop
   guard macro — `VStack(...) { children }` runs `open` once, the body once,
   `close` once, via a for-loop that only ever iterates once. This is why
   jumping out of the block with `return`/`break`/`goto` is unsafe: it skips
   the `close` the for-loop would otherwise guarantee.
2. **`EmProps`** — one designated-initializer struct every container/leaf
   accepts as `(EmProps){ .spacing = 12, ... }`; a zero field means
   "unset," which is why `EmProps` fields are never legitimately zero at
   their meaningful default (spacing/padding of exactly 0 is indistinguishable
   from "not set" — if you add a field where 0 is a valid, meaningful value,
   you need a different signal, e.g. a separate bool or a sentinel).
3. **Chainable leaves** via `EmV`, a struct-of-function-pointers. A leaf call
   like `em_text(s)` doesn't emit anything immediately — it stages the
   pending element into a single-slot buffer (`P` in `em.c`) and returns the
   `EmV` vtable; each chained method (`.caption()`, `.secondary()`) mutates
   the staged `EmProps` and returns the same vtable again. The staged element
   is only actually emitted (`em_flush()`) when the *next* leaf or container
   call starts, or when the frame ends. This deferred-emit model is why you
   never see an explicit "commit" call in app code — it's implicit in
   whatever comes next.

Everything else in `em.c` (components like `StatCard`/`Gauge`/`Dropdown`,
navigation, the animation system) is built entirely on `ui.h` calls plus
these three primitives — there's no new mechanism below this layer to learn
for a new component; see [Adding a new component](#adding-a-new-component)
below.

### `ui/dsl/em_app.c` — the app runtime

Target-only (it calls into `user/lib/embk.h`, the syscall SDK — it's part of
`libembk.so` but never linked into host tests). Two entry points,
`em_app_run` and `em_widget_run`, both driven by the `EM_APPLICATION`/
`EM_WIDGET` macros in `em.h`:

```c
#define EM_APPLICATION \
    static EmApp em_app_spec_; \
    int main(void) { return em_app_run(&em_app_spec_); } \
    static EmApp em_app_spec_ =
```

The trailing `=` is deliberate — the macro leaves the assignment open so the
user's brace-initializer (`EM_APPLICATION { .title = ..., ... };`) completes
the statement. This is why `EM_APPLICATION` must be followed by a
brace-initializer and a semicolon, and why it can only appear once per
translation unit (it defines `main`).

`em_app_run`'s flow: install the resource loader → load the font → bring up
scene/layout arenas + theme + `ui_init` → compute window geometry (clamped/
centered to the real screen size) → create the OS window
(`embk_win_create_shared_ex`, chromeless or kernel-chrome per `.chrome`) →
if chromeless, register the move callback so the app's own `WindowBar` drag
works → enter the event loop.

**The event loop is the retained-update gate**, and it's the one piece of
this file worth reading closely if you're debugging "my UI isn't updating"
or optimizing idle CPU use:

```c
int build = first || input_edge || em_take_frame_request() ||
            em_ui_epoch() != prev_epoch || em_nav_transitioning();
if (!build) { embk_sleep_ms(pace); continue; }
```

`input_edge` is computed from comparing this poll's `embk_win_input` against
last poll's (focus changed, pointer moved, buttons changed, a key arrived).
`em_take_frame_request()` reads-and-clears a flag that `Spinner`, `Toast`,
`em_animate` (while easing), and page-transition code set on every frame
they're active — that's the whole mechanism that lets an animating widget
keep frames coming while a static one goes fully idle. `em_ui_epoch()` is a
counter bumped by anything that restructures the tree in a way the app can't
see from its own input state (a dropdown toggling, a toast expiring, a tab
switching) — when it changes, the runtime also does a **full clear + full
repaint** (`scene_render_destroy`+`scene_render_init` to blow away the
dirty-rect cache, fill the buffer with the theme background), because a
structural change can vacate pixels the dirty-rect renderer has no record of
needing to erase. **If you add a new component that changes the tree shape
outside of user input**, bump `g_em_epoch` (via the same pattern `Dropdown`/
`Disclosure`/`TabView` use) or the app will silently show stale pixels in
the vacated region.

Resize handling (`em_window_take_resize`) is commit-on-release by design —
the grip accumulates a drag delta every frame while held (so the app can
show a live preview if it wants to), but only actually calls
`embk_win_resize` (which reallocates the window's shared pixel backing) once
on release, because doing that reallocation every dragged frame would thrash
the page allocator.

`em_widget_run` is the same shape, trimmed: fixed position instead of
centered/clamped, `EMBK_WINF_WIDGET` instead of chromeless-or-not, no
keyboard grab, no close-control check, and one extra retained-update trigger
(`wg->refresh_ms` timer) layered onto the identical gate.

## The compositor (kernel/gfx/compositor.c)

Everything above this line runs in userspace and knows nothing about other
windows. The compositor is the in-kernel piece that turns N independent
apps' pixel buffers into one desktop. One `struct comp_window` per window,
in a fixed-size table (`COMP_MAX_WINDOWS`):

```c
struct comp_window {
    int      pid;
    int32_t  x, y;          // frame top-left (chrome included)
    uint32_t cw, ch;         // content size
    int      z;              // paint order: higher = front
    int      desktop;        // the one full-screen back-pinned layer (home)
    int      chromeless;     // app draws its own bar/close (EmUI Window/WindowBar)
    int      widget;         // desktop widget: chromeless + its own z-band
    int      shared;         // zero-copy: content points at pages mapped into the client
    uint32_t *content;       // cw*ch premultiplied BGRA
    ...
};
```

**Three window "kinds," one struct, distinguished by flags** — `desktop`,
`chromeless`, and `widget` are not mutually exclusive tags on separate
types, they're independent booleans that each gate specific behavior
(`win_titlebar_h()` returns 0 for `desktop || chromeless`; z-order
assignment picks a different counter for `widget` vs. everything else).
Read `win_titlebar_h`, `win_close_rect`, and the z-assignment in
`win_create_shared_impl` together if you're adding a fourth kind — the
pattern is "one more boolean, one more branch in each of those three
places," not a new struct.

**Zero-copy windows**: `shared` windows don't get pixels copied in by the
compositor — the compositor allocates the physical pages, keeps a flat
kernel-side view (`kview`) for compositing, and maps the *same* pages into
the client's address space (`client_va`) so the app renders directly into
what the compositor reads. `compositor_win_resize` (V5) demonstrates the
teardown/recreate pattern for this: allocate new pages, map them into the
client at a **new** VA, swap the window's `content`/`phys`/`kview` pointers
under the compositor lock, repaint the old∪new footprint, *then* unmap and
free the old backing outside the lock. The client's old pixel pointer is
dead the instant `embk_win_resize` returns — this is a hard contract, not a
suggestion, and the app runtime handles the pointer swap for you rather than
leaving apps to get it right themselves.

**Z-bands** (added in V5): `g_next_z` and `g_widget_z` are two separate
monotonic counters. `desktop` is pinned at `z = 0`. Widgets get
`z = g_widget_z++` (a range up to `WIDGET_Z_TOP`). Regular/chromeless app
windows get `z = g_next_z++` (always above `WIDGET_Z_TOP`). Raising a window
on click re-assigns from whichever counter it already belongs to — a widget
raised by a click is still raised only within the widget band, never above
an app window. If you add a third band, add a third counter and a third
range rather than trying to interleave one counter.

**Pointer capture** (added in V5, `g_cap_pid`/`g_cap_win` in
`compositor_pointer_tick`): while the left button is held, routing sticks to
whichever window the press *landed* on, even once the cursor moves outside
that window's bounds — content-local coordinates go negative/out-of-range
during capture, which callers doing delta-based drag math (a `WindowBar`
drag, a resize grip) rely on and expect. Without this, a fast or steep drag
that outran the poll rate would have the cursor exit the 26px title bar (or
the 20px resize grip) between polls and the drag would simply die. If you
add new drag-based interaction, route it through this same capture rather
than re-deriving "am I still over the thing I grabbed."

**Click latching**: a press that lands on a window while that window's app
hasn't polled input in a while (e.g. mid multi-second first-frame render
under TCG) is *remembered* (`pend_click`) and replayed as a synthetic
press-then-release across the app's next two polls, because
`sys_win_input`/`embk_win_input` is a **state poll, not an event queue** — a
click that happens between two polls with no ongoing signal recording it
would otherwise be silently eaten.

## Adding a new component

The mechanical steps, using the existing components as the template:

1. Decide if it's a **leaf** (chainable, has an `EmV` return —
   `em_close_button`, `em_spinner`) or a **structural function** that emits
   directly (`em_gauge`, `em_stat_card`, `TabView`). Leaves stage into `P`
   via `stage(PK_YOURKIND)` and get a case in `em_flush`'s switch; structural
   functions call `em_flush()` first (to flush whatever leaf was pending) and
   then emit `ui_*` calls directly.
2. Build it out of `ui.h` primitives + existing `em_*_impl` helpers — you
   should essentially never need to touch `scene.h`/`layout.h` directly for
   a new *component* (as opposed to a new *layout primitive* or a new
   *render primitive*, which are rarer and genuinely need the lower layers).
3. Read every color/spacing/radius/font value from `TH` (the active theme,
   `em_tokens_()`/`T`), not a literal.
4. If the component restructures the tree outside of user input (opens/
   closes something, times out), bump `g_em_epoch` via `em_flush()`'s
   pattern in `Dropdown`/`Toast`/`Disclosure` so `EM_APPLICATION` knows to
   force a full repaint.
5. If it animates on its own (not driven by user interaction each frame),
   call `em_request_frame()` every frame it's visually active, the same way
   `Spinner` and `Toast` do — otherwise it'll only redraw on the next
   unrelated input edge.
6. Add it to `ui/dsl/showcase_v2.c`'s gallery so `make showcase-v2` renders
   it host-side in both themes before you ever boot a VM to check it.
7. Declare the macro + prototype in `em.h` next to its category (leaves,
   richer components, navigation) and document the signature the way the
   existing entries do — the table in [EMUI_GUIDE.md](EMUI_GUIDE.md#component-reference)
   should get a matching row.

## Testing strategy

Every layer from `scene` through `declare` has a host-compiled, host-run
test binary (`make scene-test`, `backend-test`, `font-test`, `layout-test`,
`reactive-test`, `declare-test`) — these build and run natively with the
system `HOSTCC`, no QEMU, no cross-compiler, seconds not minutes. Add
regression tests here for anything that isn't inherently about live window
management. `make showcase` / `make showcase-v2` render whole screens
(the raw kit, and the EmUI DSL/V4/V5 galleries respectively) to PNG for a
visual host-side check. Only once pixels look right in the showcase does it
make sense to boot `make run-ui` / `run-embkfs-cow` and check live
interaction, keyboard/mouse routing, and window-manager behavior that can't
be exercised outside a running compositor.
