#ifndef __EMBK_UI_PROTO_H__
#define __EMBK_UI_PROTO_H__

/* user/lib/embk_ui_proto.h -- EmbLink UI Piece 2: the compositor protocol.
 *
 * THIS IS THE WIRE FORMAT. It rides entirely on Piece 1's channel primitives
 * (chan_send/chan_recv payload + ancillary handles) and introduces no new
 * kernel mechanism -- only a message vocabulary. Client SDK and compositor
 * both #include this one header so the two sides can never silently drift
 * (unlike the kernel/userland split spawn_file_action had: here there is
 * exactly ONE file, included by both, so there is nothing to hand-sync).
 *
 * Dispatch model (Piece 2 Section 0/1): ASYNC + TAGGED. A channel end has one
 * FIFO inbox shared by window-management replies AND a continuous input-event
 * stream, so no message is ever "the reply to my last call": every message
 * self-describes via `type` + `request_id`, and a client matches replies by
 * echoed request_id from wherever they land, dispatching everything else
 * (request_id == 0) as pure events. This is exactly Wayland's model, for
 * exactly this reason. */

#include <stdint.h>

#define EMBK_UI_PROTOCOL_VERSION 1

/* Protocol status codes carried in reply payloads (msg_hello_ack.status,
 * msg_role_created.status, msg_error.code). These MIRROR the negative-errno
 * values in kernel/include/errno.h -- userland roles (user/bin/init.c) are
 * freestanding and don't include that kernel header, so the few the protocol
 * needs are restated here. Guarded so a translation unit that DID pull in the
 * kernel definitions (or a future userland errno.h) doesn't collide. */
#ifndef EMBK_EAGAIN
#define EMBK_EAGAIN  11
#endif
#ifndef EMBK_EINVAL
#define EMBK_EINVAL  22
#endif
#ifndef EMBK_EACCES
#define EMBK_EACCES  13
#endif
#ifndef EMBK_EPIPE
#define EMBK_EPIPE   32
#endif
#ifndef EMBK_EPROTO
#define EMBK_EPROTO  71
#endif

/* ------------------------------------------------------------------------- */
/* Section 2: message types                                                  */
/* ------------------------------------------------------------------------- */

enum msg_type {
    MSG_NONE = 0,

    /* connection-level, window_id == 0 */
    MSG_HELLO          = 1,   /* client -> compositor, request */
    MSG_HELLO_ACK      = 2,   /* compositor -> client, reply */
    MSG_ERROR          = 3,   /* compositor -> client, either-triggered */

    /* window lifecycle */
    MSG_CREATE_SURFACE_ROLE = 10,  /* client -> compositor, request */
    MSG_ROLE_CREATED        = 11,  /* compositor -> client, reply */
    MSG_SET_TITLE           = 12,  /* client -> compositor, event (APP_WINDOW only) */
    MSG_DESTROY_ROLE        = 13,  /* client -> compositor, event */
    MSG_CONFIGURE           = 14,  /* compositor -> client, event */
    MSG_CLOSE_REQUESTED     = 15,  /* compositor -> client, event (advisory) */

    /* frame lifecycle */
    MSG_SURFACE_ATTACH  = 20,  /* client -> compositor, event; 1 ancillary handle */
    MSG_FRAME_COMMIT    = 21,  /* client -> compositor, event, per-frame */
    MSG_FRAME_DONE      = 22,  /* compositor -> client, event, pacing signal */

    /* input, compositor -> client, always event (request_id=0), window-scoped */
    MSG_POINTER_ENTER   = 30,
    MSG_POINTER_LEAVE   = 31,
    MSG_POINTER_MOTION  = 32,
    MSG_POINTER_BUTTON  = 33,
    MSG_POINTER_SCROLL  = 34,
    MSG_KEY             = 35,
    MSG_FOCUS_ENTER     = 36,
    MSG_FOCUS_LEAVE     = 37,
};

/* Fixed 16-byte header, followed by a type-specific payload. One message ==
 * one chan_send: the payload IS the message body; ancillary handles (when a
 * type carries them) ride the same chan_send call (Piece 1 Section A.3). */
struct msg_header {
    uint16_t type;          /* enum msg_type */
    uint16_t reserved;      /* zero; future use */
    uint32_t request_id;    /* 0 = pure event, dispatched immediately (P1).
                             * Nonzero on a request = sender-chosen correlation
                             * id, ECHOED VERBATIM by the matching reply. At
                             * most once among a connection's outstanding
                             * requests (P2); the client must not reuse an id
                             * while a request bearing it is unreplied. */
    uint32_t window_id;     /* 0 = connection-level (HELLO/ERROR at connect
                             * time). Nonzero = which window this concerns. */
    uint32_t payload_len;   /* SANITY CHECK ONLY. chan_recv's out_len is the
                             * authoritative boundary; a receiver asserts
                             * payload_len + sizeof(header) == out_len to catch
                             * a corrupt/truncated build early rather than
                             * misread a mis-sized payload as the wrong struct. */
};

/* Payload structs are cast directly onto (buf + sizeof(struct msg_header)). */

/* ------------------------------------------------------------------------- */
/* Section 3: handshake                                                      */
/* ------------------------------------------------------------------------- */

struct msg_hello {              /* client -> compositor. request_id chosen. window_id=0. */
    uint32_t client_version;    /* must == EMBK_UI_PROTOCOL_VERSION today */
};

struct msg_hello_ack {          /* compositor -> client. request_id echoes. window_id=0. */
    int32_t  status;            /* 0 ok; -EMBK_EPROTO on version mismatch */
    uint32_t compositor_version;
};

/* ------------------------------------------------------------------------- */
/* Section 4: surface roles & window creation                                */
/* ------------------------------------------------------------------------- */

enum surface_role {
    ROLE_NONE       = 0,
    ROLE_APP_WINDOW = 1,  /* any client, via /run/compositor */
    ROLE_BACKGROUND = 2,  /* shell only, via /run/compositor-shell; exactly ONE */
    ROLE_PANEL      = 3,  /* shell only, via /run/compositor-shell; always above APP z */
};

struct msg_create_surface_role {  /* client -> compositor. request_id chosen. window_id=0. */
    uint32_t role;                 /* enum surface_role */
    uint32_t requested_width, requested_height;  /* hint; compositor may override */
};

struct msg_role_created {         /* compositor -> client. request_id echoes. */
    int32_t  status;              /* 0 ok; -EMBK_EACCES (role needs shell privilege and
                                   * this isn't the shell connection); -EMBK_EINVAL
                                   * (e.g. a second BACKGROUND) */
    uint32_t window_id;            /* newly assigned; 0 if status != 0 */
    uint32_t assigned_width, assigned_height;
};

struct msg_set_title {     /* client -> compositor, event. window_id set. APP_WINDOW only. */
    char title[64];         /* NUL-terminated, truncated if longer */
};

struct msg_destroy_role { char _unused; }; /* event. window_id set. No real payload. */

/* ------------------------------------------------------------------------- */
/* Section 5: frame lifecycle                                                */
/* ------------------------------------------------------------------------- */

struct msg_surface_attach {  /* client -> compositor, event. window_id set. Carries
                              * exactly ONE ancillary handle: the surface, COPY
                              * (client keeps drawing). Sent when a window's surface
                              * is first established, and again on any CONFIGURE size
                              * change (surfaces are fixed-size; resize = NEW surface
                              * + re-attach). No byte payload beyond the header. */
    char _unused;
};

struct msg_frame_commit {    /* client -> compositor, event, PER FRAME. window_id set. */
    uint32_t buffer_idx;      /* which buffer of the attached surface was just
                              * surface_commit()'d at the syscall layer */
    int32_t  damage_x, damage_y, damage_w, damage_h;
                              /* hint only; (0,0,0,0) or negative w/h == whole buffer */
};

struct msg_frame_done { char _unused; };  /* compositor -> client, event. window_id set.
                              * Sent once the compositor has actually called
                              * surface_release() on the buffer from the most recent
                              * FRAME_COMMIT (P5). PACING ONLY -- acquire() is always
                              * safe (returns -EAGAIN if nothing free); this just lets
                              * a client block in its loop instead of spin-polling. */

/* ------------------------------------------------------------------------- */
/* Section 6: window management (compositor -> client)                       */
/* ------------------------------------------------------------------------- */

enum window_state_flags {
    WIN_STATE_NONE      = 0,
    WIN_STATE_ACTIVATED = 1 << 0,  /* has keyboard focus */
    WIN_STATE_MAXIMIZED = 1 << 1,
};

struct msg_configure {      /* compositor -> client, event. window_id set. */
    uint32_t width, height;  /* size the compositor wants this window's NEXT surface
                              * to be; client surface_creates at this size + re-ATTACHes
                              * (Piece 1 has no in-place resize). */
    uint32_t state;          /* enum window_state_flags */
};

struct msg_close_requested { char _unused; }; /* compositor -> client, event. window_id set.
                              * ADVISORY -- the compositor does NOT force-terminate; the
                              * client decides whether/when to DESTROY_ROLE + exit. */

/* ------------------------------------------------------------------------- */
/* Section 7: errors                                                         */
/* ------------------------------------------------------------------------- */

struct msg_error {          /* compositor -> client. window_id = offending window (or 0).
                             * request_id echoes the offending request (0 otherwise). */
    int32_t  code;           /* -EMBK_Exxx */
    char     detail[64];     /* short NUL-terminated context */
};

/* ------------------------------------------------------------------------- */
/* Section 8: input events (compositor -> client, request_id=0, window-scoped)*/
/* ------------------------------------------------------------------------- */

struct msg_pointer_enter  { int32_t x, y; };  /* window-LOCAL surface coords */
struct msg_pointer_leave  { char _unused; };
struct msg_pointer_motion { int32_t x, y; };  /* window-LOCAL, always */

enum pointer_button { PTR_BTN_LEFT = 0, PTR_BTN_RIGHT = 1, PTR_BTN_MIDDLE = 2 };
struct msg_pointer_button {
    uint32_t button;   /* enum pointer_button */
    uint32_t pressed;  /* 1 down, 0 up */
    int32_t  x, y;     /* window-local, at time of click */
};

struct msg_pointer_scroll { int32_t dx, dy; };  /* compositor-defined units */

struct msg_key {
    uint32_t keycode;    /* RAW scancode -- keymap/unicode is a separate later piece */
    uint32_t pressed;
    uint32_t modifiers;  /* bitmask, defined alongside a real keymap later */
};

struct msg_focus_enter { char _unused; };  /* window_id now has keyboard focus */
struct msg_focus_leave { char _unused; };

/* ------------------------------------------------------------------------- */
/* Convenience: build/parse the envelope. Header-only, freestanding-safe.     */
/* Not part of the wire spec -- just so both sides frame messages identically.*/
/* ------------------------------------------------------------------------- */

#define EMBK_UI_MSG_MAX 256   /* largest header+payload any type here needs */

/* Copy `n` bytes src->dst (freestanding; no libc dependency here). */
static inline void embk_ui_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

/* Assemble header+payload into `out` (>= EMBK_UI_MSG_MAX). Returns total len. */
static inline unsigned embk_ui_pack(void *out, uint16_t type, uint32_t request_id,
                                    uint32_t window_id, const void *payload,
                                    uint32_t payload_len) {
    struct msg_header h;
    h.type = type; h.reserved = 0; h.request_id = request_id;
    h.window_id = window_id; h.payload_len = payload_len;
    embk_ui_memcpy(out, &h, sizeof(h));
    if (payload_len && payload)
        embk_ui_memcpy((uint8_t *)out + sizeof(h), payload, payload_len);
    return (unsigned)(sizeof(h) + payload_len);
}

/* Validate a freshly-recv'd message: `total` is chan_recv's out_len (the
 * authoritative boundary). Returns 1 iff it's a well-formed envelope whose
 * header.payload_len agrees with `total` (the P-side sanity check). */
static inline int embk_ui_check(const void *buf, unsigned total) {
    if (total < sizeof(struct msg_header)) return 0;
    const struct msg_header *h = (const struct msg_header *)buf;
    return (h->payload_len + sizeof(struct msg_header)) == total;
}

/* Pointer to the payload region of a received message. */
static inline const void *embk_ui_payload(const void *buf) {
    return (const uint8_t *)buf + sizeof(struct msg_header);
}

#endif /* __EMBK_UI_PROTO_H__ */
