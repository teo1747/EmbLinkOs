#ifndef __EMBLINK_UI_UI_H__
#define __EMBLINK_UI_UI_H__

/* ui/declare/ui.h -- EmbLink UI Piece 7: the declarative API an app author
 * writes against. Begin/end imperative scoping (Dear ImGui family); positional
 * identity by default, explicit keys for dynamic lists. */

#include "instance.h"

/* --- driver / frame loop glue --- */
void ui_init(struct scene_arena *sa, struct layout_arena *la);
struct instance_handle ui_root(void);
void ui_frame_begin(void);
void ui_frame_end(void);
void ui_run_layout(float W, float H);   /* Piece 5 arrange on the root */
void ui_set_font(uint32_t font_handle);
void ui_set_text_size(float px);

/* --- containers --- */
void ui_box_begin(uint64_t key);
void ui_box_end(void);
void ui_begin_vstack(uint64_t key);
void ui_begin_hstack(uint64_t key);
void ui_end_stack(void);

/* --- properties (apply to the currently open box) --- */
void ui_set_paint(struct paint p);
void ui_set_corner_radius(float r);
void ui_set_padding(float top, float right, float bottom, float left);
void ui_set_spacing(float s);
void ui_set_size(struct layout_size w, struct layout_size h);
void ui_set_shadow(bool enabled, float dx, float dy, float blur, struct color color);
void ui_set_opacity(float opacity);   /* 0..1; wraps the subtree in a group */
void ui_set_offset(float x, float y);  /* post-layout translate (transitions/slides) */
void ui_set_backdrop_blur(bool enabled, float radius);
void ui_set_clip_children(bool clip);
void ui_set_overlay(bool on);   /* fill parent, out of flow (modal/popover layer) */
void ui_set_border(float width, struct color color);
void ui_set_axis(enum layout_axis a);
void ui_set_justify(enum layout_justify j);
void ui_set_align(enum layout_align a);
void ui_set_text_color(struct color c);   /* colour for subsequent ui_text calls */

/* --- leaves --- */
void ui_text(const char *fmt, ...);
void ui_image(uint64_t key, const void *pixels, uint32_t iw, uint32_t ih, float height_px);
void ui_text_keyed(uint64_t key, const char *fmt, ...);
void ui_spacer(void);
bool ui_button(const char *label);
bool ui_button_keyed(uint64_t key, const char *label);

void ui_component(void (*fn)(void *props), const void *props, size_t props_size, uint64_t key);

/* --- input dispatch (§7): hit-test the retained tree, set clicked pulse --- */
void ui_dispatch_click(float px, float py);

/* Handle of the currently open box (for widget authors that need to read back
 * interaction state), and read+clear its one-frame click pulse. */
struct instance_handle ui_open(void);
bool ui_consume_click(struct instance_handle h);

/* Live pointer input for the event loop: feed the pointer each frame (coords
 * are surface-local), then widgets query hover/press for the currently open
 * box. A click fires on the press edge and is read via ui_consume_click. */
void ui_pointer(float x, float y, bool down);
void ui_pointer_pos(float *x, float *y);   /* live pointer position (surface-local) */
bool ui_is_hovered(void);   /* is the pointer over the currently open box? */
bool ui_is_pressed(void);   /* ...and is the button held down? */
bool ui_is_active(void);    /* is the open box the drag owner (pointer capture)? */

/* World rect (last frame's arranged geometry) of an instance / the open box.
 * Widgets map the pointer into their own box with these (sliders, scrollables). */
bool ui_rect_of(struct instance_handle h, float *x, float *y, float *w, float *ht);
bool ui_open_rect(float *x, float *y, float *w, float *ht);

/* --- keyboard focus + typed input (text fields) --- */
void ui_input_char(int c);                        /* loop feeds each key byte here */
int  ui_input_take(char *dst, int max);           /* focused field drains queued chars */
void ui_request_focus(struct instance_handle h);
bool ui_has_focus(struct instance_handle h);

/* --- scroll --- */
void  ui_set_scroll_offset(float dy);             /* shift open box's children up by dy */
bool  ui_open_content_extent(float *content_h, float *viewport_h);
void  ui_wheel(float dy);                         /* loop feeds this frame's wheel delta */
float ui_take_wheel(void);                        /* open box consumes wheel if hovered */

/* --- test/diagnostic --- */
uint32_t ui_debug_mutation_count(void);
struct instance_handle ui_first_child(struct instance_handle h);
struct instance_handle ui_next_sibling(struct instance_handle h);
struct node_handle     ui_scene_of(struct instance_handle h);
struct layout_handle   ui_layout_of(struct instance_handle h);

#endif /* __EMBLINK_UI_UI_H__ */
