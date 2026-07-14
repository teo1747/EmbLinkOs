/* user/init.c — the first userland program and the EmbLink native-primitive
 * self-test. Freestanding: its own _start (no crt0), no libc. Built entirely
 * on the EmbLink SDK (user/embk.h) rather than hand-rolled syscalls, so it
 * doubles as the SDK's own dogfooding. Linked at 0x400000 (user/user.ld).
 *
 * TWO ROLES, selected by argc (this is load-bearing -- see _start):
 *   - Top level (argc == 1, e.g. `run /init.elf` or `test ring3 threads`):
 *     run the full native-primitive suite (threads, spawn, sbrk) and exit
 *     16 iff everything passed. 16 is the success sentinel the kernel's
 *     `test ring3 threads` selftest asserts on.
 *   - Spawned child (argc >= 2): the minimal behavior the parent's
 *     spawn_test() checks -- echo argv[1] to fd 3 (a file the parent
 *     redirected in via a spawn file-action) and exit argc. MUST exit here
 *     and never fall through into the suite, or it would recursively spawn
 *     itself forever. */

#include "embk.h"

/* --- shared state the thread test proves is genuinely shared ------------ */
volatile int  counter    = 5;   /* .data: mutated by the second thread below */
volatile int  thread_ran = 0;   /* set ONLY by second_thread_entry -- proof
                                  * _start observes the other thread's writes */

/* Runs as an ADDITIONAL thread of this same process (embk_thread_create),
 * on its own user stack but the exact same address space as _start. Its
 * writes to counter/thread_ran must be visible to _start after the join, or
 * the two "threads" don't actually share memory. `arg` arrives in RDI as
 * this function's first parameter. */
static void second_thread_entry(long arg)
{
    embk_puts(1, "Hello from thread 2!\n");
    thread_ran = 1;
    counter += (int)arg;
    embk_thread_exit(99);
}

/* thread_create + join, then verify the shared-memory side effects. Returns
 * 1 on pass, 0 on fail. */
static int thread_test(void)
{
    int start = counter;
    long tid = embk_thread_create(second_thread_entry, 11);
    if (tid < 0) { embk_puts(1, "thread_test: FAIL create\n"); return 0; }

    long code = embk_thread_join((int)tid);
    if (code != 99) { embk_puts(1, "thread_test: FAIL join code\n"); return 0; }

    if (!thread_ran || counter != start + 11) {
        embk_puts(1, "thread_test: FAIL shared memory not observed\n");
        return 0;
    }
    embk_puts(1, "thread_test: PASS\n");
    return 1;
}

/* Spawn a child (this same ELF, argc>=2 path) with argv AND a file-action
 * that pre-opens /spawn_test.tmp onto the child's fd 3; the child writes
 * argv[1] there. Then reopen the file and confirm the bytes round-tripped.
 * Proves argv delivery, spawn file-actions, wait, and the fd/VFS path all
 * work together. Returns 1 on pass, 0 on fail. */
static int spawn_test(void)
{
    char *argv[] = { "init", "hello-from-argv", (char *)0 };

    struct embk_spawn_file_action actions[1];
    actions[0].kind = EMBK_SPAWN_ACTION_OPEN;
    actions[0].target_fd = 3;
    { const char *p = "/spawn_test.tmp"; int i = 0;
      while (p[i]) { actions[0].path[i] = p[i]; i++; }
      actions[0].path[i] = '\0'; }
    actions[0].flags = EMBK_O_CREAT | EMBK_O_WRONLY | EMBK_O_TRUNC;
    actions[0].mode = 0644;

    long handle = embk_spawn("/init.elf", argv, actions, 1);
    if (handle < 0) { embk_puts(1, "spawn_test: FAIL spawn()\n"); return 0; }

    long exit_code = embk_wait((int)handle);
    if (exit_code != 2) { embk_puts(1, "spawn_test: FAIL wrong argc\n"); return 0; }

    long fd = embk_open("/spawn_test.tmp", EMBK_O_RDONLY, 0);
    if (fd < 0) { embk_puts(1, "spawn_test: FAIL reopen\n"); return 0; }
    char buf[64] = {0};
    long n = embk_read((int)fd, buf, sizeof(buf) - 1);
    embk_close((int)fd);

    if (n <= 0 || !embk_streq(buf, "hello-from-argv")) {
        embk_puts(1, "spawn_test: FAIL content mismatch\n");
        return 0;
    }
    embk_puts(1, "spawn_test: PASS\n");
    return 1;
}

#define EXPECTED_HEAP_BASE 0x0000600000000000L

/* Grow/shrink the heap via embk_sbrk and verify the mapping, persistence,
 * shrink-doesn't-unmap, and oversized-request-rejection behavior. Returns 1
 * on pass, 0 on fail. */
static int sbrk_test(void)
{
    long base = embk_sbrk(0);
    if (base != EXPECTED_HEAP_BASE) { embk_puts(1, "sbrk_test: FAIL initial break\n"); return 0; }

    long grow = 3 * 4096;   /* span three pages: exercises the mapping LOOP */
    long old_brk = embk_sbrk(grow);
    if (old_brk != base) { embk_puts(1, "sbrk_test: FAIL didn't return old break\n"); return 0; }

    /* Touch every page (a #PF here would be a loud, unambiguous fail). */
    volatile char *heap = (volatile char *)base;
    for (long i = 0; i < grow; i += 512) heap[i] = (char)(i & 0xFF);
    for (long i = 0; i < grow; i += 512) {
        if (heap[i] != (char)(i & 0xFF)) { embk_puts(1, "sbrk_test: FAIL data mismatch\n"); return 0; }
    }
    if (embk_sbrk(0) != base + grow) { embk_puts(1, "sbrk_test: FAIL break didn't persist\n"); return 0; }

    /* Shrink one page then regrow -- the regrown page must still hold its
     * bytes (heap_mapped_top never unmaps on shrink). */
    (void)embk_sbrk(-4096);
    if (embk_sbrk(0) != base + grow - 4096) { embk_puts(1, "sbrk_test: FAIL shrink\n"); return 0; }
    (void)embk_sbrk(4096);
    if (heap[grow - 4096] != (char)((grow - 4096) & 0xFF)) {
        embk_puts(1, "sbrk_test: FAIL regrow lost data\n"); return 0;
    }

    /* Absurd request must fail cleanly and not disturb the break. */
    long huge = embk_sbrk(0x0000000100000000L);   /* 4 GiB, past the 1 GiB cap */
    if (huge >= 0) { embk_puts(1, "sbrk_test: FAIL oversized didn't fail\n"); return 0; }
    if (embk_sbrk(0) != base + grow) { embk_puts(1, "sbrk_test: FAIL failed req corrupted break\n"); return 0; }

    embk_puts(1, "sbrk_test: PASS\n");
    return 1;
}

/* ---- EmbLink UI Piece 1: surface / shared-memory selftests ------------- */
#define SURF_PATTERN 0xAABBCCDDu

/* S2/S3, single address space: the buffer-ownership state machine and the
 * starvation/newest-wins handshake -- these are kernel-side invariants that
 * don't need a second process. Returns 1 on pass, 0 on fail. */
static int surface_ownership_test(void)
{
    struct embk_surface_info info;
    int s = embk_surface_create(4, 4, EMBK_PIXFMT_BGRA8888_PRE, 2, &info);
    if (s < 0) { embk_puts(1, "surface: create(ownership) FAIL\n"); return 0; }

    int a = embk_surface_acquire(s);                    /* first free -> buffer 0 */
    if (a != 0) { embk_puts(1, "surface: acquire#1 FAIL\n"); embk_surface_destroy(s); return 0; }
    if (embk_surface_commit(s, a) != 0) { embk_puts(1, "surface: commit#1 FAIL\n"); embk_surface_destroy(s); return 0; }

    /* S2: re-committing a buffer we no longer own must be rejected. */
    if (embk_surface_commit(s, a) >= 0) { embk_puts(1, "surface: recommit-not-rejected FAIL\n"); embk_surface_destroy(s); return 0; }

    int b = embk_surface_acquire(s);                    /* must be the OTHER buffer */
    if (b != 1) { embk_puts(1, "surface: acquire#2-not-other FAIL\n"); embk_surface_destroy(s); return 0; }
    if (embk_surface_commit(s, b) != 0) { embk_puts(1, "surface: commit#2 FAIL\n"); embk_surface_destroy(s); return 0; }

    /* S3: both compositor-owned now -> acquire must starve. */
    if (embk_surface_acquire(s) >= 0) { embk_puts(1, "surface: not-starved FAIL\n"); embk_surface_destroy(s); return 0; }

    /* release buffer 0 -> the next acquire hands it back. */
    if (embk_surface_release(s, 0) != 0) { embk_puts(1, "surface: release FAIL\n"); embk_surface_destroy(s); return 0; }
    if (embk_surface_acquire(s) != 0) { embk_puts(1, "surface: reacquire FAIL\n"); embk_surface_destroy(s); return 0; }

    embk_surface_destroy(s);
    embk_puts(1, "surface: ownership (S2/S3) PASS\n");
    return 1;
}

/* Spawned child (argv[1]=="surface-child"): map the surface it INHERITED at
 * spawn (obj-handle 0), read pixel 0 of buffer `idx` (argv[2]) from its OWN
 * address space, and exit 42 iff it sees the pattern the parent wrote. That
 * exit code is the whole proof of S1: two address spaces, one physical page. */
static void surface_child(long argc, char **argv)
{
    int idx = (argc >= 3 && argv[2] && argv[2][0]) ? (argv[2][0] - '0') : 0;
    struct embk_surface_info info;
    long base = embk_surface_map(0, &info);
    if (base < 0) embk_exit(1);
    volatile unsigned *px = (volatile unsigned *)(base + (long)idx * (long)info.buffer_size);
    embk_exit(px[0] == SURF_PATTERN ? 42 : 1);
}

/* Parent (argv[1]=="surface-parent"): run S2/S3, then S1 -- write a known
 * pattern into a surface, spawn a child that INHERITS it and reads the
 * pattern back cross-address-space. Exit 55 iff everything passed. Does NOT
 * destroy the S1 surface: the kernel selftest then checks the live-surface
 * count returns to baseline, proving R2 frees it on process exit. */
static void surface_parent(void)
{
    if (!surface_ownership_test()) embk_exit(2);

    struct embk_surface_info info;
    int s = embk_surface_create(4, 4, EMBK_PIXFMT_BGRA8888_PRE, 2, &info);
    if (s < 0) { embk_puts(1, "surface: create(S1) FAIL\n"); embk_exit(3); }

    int idx = embk_surface_acquire(s);
    if (idx < 0) { embk_puts(1, "surface: acquire(S1) FAIL\n"); embk_exit(3); }
    long base = embk_surface_map(s, &info);   /* learn the base VA (already mapped) */
    if (base < 0) { embk_puts(1, "surface: map(S1) FAIL\n"); embk_exit(3); }

    volatile unsigned *px = (volatile unsigned *)(base + (long)idx * (long)info.buffer_size);
    px[0] = SURF_PATTERN;
    if (embk_surface_commit(s, idx) != 0) { embk_puts(1, "surface: commit(S1) FAIL\n"); embk_exit(3); }

    char idxbuf[2] = { (char)('0' + idx), 0 };
    char *cargv[] = { "/init.elf", "surface-child", idxbuf, (char *)0 };
    struct embk_spawn_file_action act[1];
    act[0].kind = EMBK_SPAWN_ACTION_INHERIT_SURFACE;
    act[0].target_fd = s;                     /* hand the child THIS surface */
    long h = embk_spawn("/init.elf", cargv, act, 1);
    if (h < 0) { embk_puts(1, "surface: spawn child FAIL\n"); embk_exit(3); }
    long code = embk_wait((int)h);
    if (code != 42) { embk_puts(1, "surface: child-didn't-see-pixel FAIL\n"); embk_exit(3); }

    embk_puts(1, "surface: cross-addr-space (S1) PASS\n");
    embk_exit(55);
}

/* ---- EmbLink UI Piece 1, Layer A: IPC channel selftests ---------------- */
#define CHAN_N 48   /* > CHAN_QUEUE_MAX (32): forces the backpressure path */

static volatile int chan_recv_ok    = 0;
static volatile int chan_recv_count = 0;

/* Runs as a second thread: drains CHAN_N messages, checking each is exactly
 * one byte equal to its index (boundaries A1) -- blocking on empty (A2) as
 * the main thread fills, which itself blocks when the queue caps (A3). */
static void chan_recv_thread(long h1)
{
    unsigned char buf[8];
    int ok = 1;
    for (int i = 0; i < CHAN_N; i++) {
        unsigned len = 0, nh = 0;
        int rc = embk_chan_recv((int)h1, buf, sizeof(buf), &len, 0, &nh);
        if (rc < 0 || len != 1 || buf[0] != (unsigned char)i) { ok = 0; break; }
        chan_recv_count++;
    }
    chan_recv_ok = ok;
    embk_thread_exit(0);
}

/* A1/A2/A3: send CHAN_N distinct one-byte messages while a second thread
 * recvs them; all must arrive, in order, exactly (queue cap forces blocking
 * both ways). */
static int chan_flow_test(void)
{
    int h[2];
    if (embk_chan_pair(h) < 0) { embk_puts(1, "chan: pair FAIL\n"); return 0; }
    long tid = embk_thread_create(chan_recv_thread, h[1]);
    if (tid < 0) { embk_puts(1, "chan: thread FAIL\n"); return 0; }

    for (int i = 0; i < CHAN_N; i++) {
        unsigned char b = (unsigned char)i;
        if (embk_chan_send(h[0], &b, 1, 0, 0, 0) < 0) { embk_puts(1, "chan: send FAIL\n"); return 0; }
    }
    embk_thread_join((int)tid);
    embk_chan_close(h[0]);
    embk_chan_close(h[1]);

    if (!chan_recv_ok || chan_recv_count != CHAN_N) { embk_puts(1, "chan: flow FAIL\n"); return 0; }
    embk_puts(1, "chan: boundaries+blocking+backpressure (A1/A2/A3) PASS\n");
    return 1;
}

/* A4 + message size: peer-close gives EPIPE; an oversized recv buffer gives
 * EMSGSIZE and leaves the message queued. */
static int chan_close_test(void)
{
    int h[2];
    if (embk_chan_pair(h) < 0) { embk_puts(1, "chan: pair2 FAIL\n"); return 0; }

    /* EMSGSIZE: send 3 bytes, recv with a 1-byte cap -> EMSGSIZE, still queued. */
    if (embk_chan_send(h[0], "abc", 3, 0, 0, 0) < 0) { embk_puts(1, "chan: send3 FAIL\n"); return 0; }
    unsigned char small[1]; unsigned len = 0, nh = 0;
    if (embk_chan_recv(h[1], small, 1, &len, 0, &nh) >= 0) { embk_puts(1, "chan: EMSGSIZE not raised FAIL\n"); return 0; }
    unsigned char big[8];
    if (embk_chan_recv(h[1], big, sizeof(big), &len, 0, &nh) < 0 || len != 3) { embk_puts(1, "chan: refetch FAIL\n"); return 0; }

    /* A4: close h0; recv on h1 (now empty + peer closed) -> error (EPIPE). */
    embk_chan_close(h[0]);
    if (embk_chan_recv(h[1], big, sizeof(big), &len, 0, &nh) >= 0) {
        embk_puts(1, "chan: EPIPE not raised FAIL\n"); embk_chan_close(h[1]); return 0;
    }
    embk_chan_close(h[1]);
    embk_puts(1, "chan: peer-close (A4) + EMSGSIZE PASS\n");
    return 1;
}

/* S-chan-3: send a surface handle COPY, then close the channel BEFORE anyone
 * recvs it. The in-transit ref must be released (A6). We then destroy our own
 * handle; the kernel selftest verifies the live-surface count returned to its
 * baseline -- i.e. the undelivered handoff didn't leak the surface. */
static int chan_handle_refcount_test(void)
{
    struct embk_surface_info info;
    int s = embk_surface_create(4, 4, EMBK_PIXFMT_BGRA8888_PRE, 2, &info);
    if (s < 0) { embk_puts(1, "chan: rc-create FAIL\n"); return 0; }

    int h[2];
    if (embk_chan_pair(h) < 0) { embk_surface_destroy(s); embk_puts(1, "chan: rc-pair FAIL\n"); return 0; }

    int hnd = s, fl = EMBK_CHAN_HANDLE_COPY;
    if (embk_chan_send(h[0], "x", 1, &hnd, &fl, 1) < 0) { embk_puts(1, "chan: rc-send FAIL\n"); return 0; }

    embk_chan_close(h[1]);   /* drains the undelivered msg -> releases in-transit ref (A6) */
    embk_chan_close(h[0]);
    embk_surface_destroy(s); /* now refcount should hit 0 iff A6 released cleanly */

    embk_puts(1, "chan: handle-refcount-across-queue (S-chan-3) done\n");
    return 1;
}

static void chan_role(void)
{
    int ok = 1;
    ok &= chan_flow_test();
    ok &= chan_close_test();
    ok &= chan_handle_refcount_test();
    embk_exit(ok ? 77 : 1);
}

/* ---- EmbLink UI Piece 1, Layers B+C: rendezvous + the real surface-over-  */
/* channel handoff (spec C.5's compositor loop, for real). Neither role      */
/* explicitly destroys/closes anything -- both just exit, relying ENTIRELY   */
/* on R2 process-exit cleanup (obj_handles_release_all) to unmap surfaces,   */
/* close channel ends, and unlink the epfs endpoint. The kernel selftest     */
/* wrapper (test compositor) checks live-surface/live-channel counts return  */
/* to baseline after both processes are reaped -- the strongest form of the  */
/* crash-safety claim (R2/R3): a full real-world session's worth of shared   */
/* state, cleaned up automatically, zero manual teardown calls. ------------ */

#define SURF_PATTERN_1 0xAABBCCDDu
#define SURF_PATTERN_2 0x11223344u

/* Client: connect, create+commit a surface, send it COPY ("attach"), create
 * a second surface and send it MOVE ("move") -- then verify locally that the
 * MOVE actually consumed its own handle (S-move's sender-side half). */
static void compositor_client_role(void)
{
    int ch = embk_chan_connect("/run/compositor");
    if (ch < 0) embk_exit(2);

    struct embk_surface_info info;
    int s1 = embk_surface_create(4, 4, EMBK_PIXFMT_BGRA8888_PRE, 2, &info);
    if (s1 < 0) embk_exit(3);
    int idx1 = embk_surface_acquire(s1);
    if (idx1 < 0) embk_exit(3);
    long base1 = embk_surface_map(s1, &info);
    if (base1 < 0) embk_exit(3);
    ((volatile unsigned *)base1)[0] = SURF_PATTERN_1;
    if (embk_surface_commit(s1, idx1) != 0) embk_exit(3);

    int h1 = s1, f1 = EMBK_CHAN_HANDLE_COPY;
    if (embk_chan_send(ch, "attach", 6, &h1, &f1, 1) < 0) embk_exit(4);

    int s2 = embk_surface_create(4, 4, EMBK_PIXFMT_BGRA8888_PRE, 2, &info);
    if (s2 < 0) embk_exit(5);
    int idx2 = embk_surface_acquire(s2);
    if (idx2 < 0) embk_exit(5);
    long base2 = embk_surface_map(s2, &info);
    if (base2 < 0) embk_exit(5);
    ((volatile unsigned *)base2)[0] = SURF_PATTERN_2;
    if (embk_surface_commit(s2, idx2) != 0) embk_exit(5);

    int h2 = s2, f2 = EMBK_CHAN_HANDLE_MOVE;
    if (embk_chan_send(ch, "move", 4, &h2, &f2, 1) < 0) embk_exit(6);

    /* S-move (sender side): after a MOVE send, the sender's own handle must
     * be gone -- any further op on it resolves to nothing. */
    if (embk_surface_acquire(s2) >= 0) { embk_puts(1, "client: S-move FAIL (handle still live)\n"); embk_exit(7); }

    embk_puts(1, "client: attach (COPY) + move (MOVE) sent, S-move sender-side PASS\n");
    embk_exit(66);   /* deliberately no chan_close/surface_destroy -- see R2 note above */
}

/* Compositor: listen, spawn the client, accept, receive both surfaces,
 * verify S-surf-1 (cross-address-space pattern) on the COPY'd one, run
 * S-surf-2/S-surf-3 (ownership + starvation) on it, then verify the MOVE'd
 * one arrived correctly too. Exits 88 iff everything passed. */
static void compositor_role(void)
{
    int lh = embk_chan_listen("/run/compositor");
    if (lh < 0) { embk_puts(1, "compositor: listen FAIL\n"); embk_exit(2); }

    char *cargv[] = { "init", "compositor-client", (char *)0 };
    long spawn_h = embk_spawn("/init.elf", cargv, 0, 0);
    if (spawn_h < 0) { embk_puts(1, "compositor: spawn FAIL\n"); embk_exit(3); }

    int ch = embk_chan_accept(lh);
    if (ch < 0) { embk_puts(1, "compositor: accept FAIL\n"); embk_exit(4); }

    /* ---- "attach": S-surf-1, the core cross-address-space claim ---- */
    char buf[16]; unsigned len = 0, nh = 0; int hnds[4];
    if (embk_chan_recv(ch, buf, sizeof(buf), &len, hnds, &nh) < 0 || nh != 1) {
        embk_puts(1, "compositor: recv(attach) FAIL\n"); embk_exit(5);
    }
    int s1 = hnds[0];
    struct embk_surface_info info;
    long base1 = embk_surface_map(s1, &info);   /* the step spec C.4 leaves to the receiver */
    if (base1 < 0) { embk_puts(1, "compositor: map(s1) FAIL\n"); embk_exit(5); }
    if (((volatile unsigned *)base1)[0] != SURF_PATTERN_1) {
        embk_puts(1, "compositor: S-surf-1 FAIL (pattern mismatch)\n"); embk_exit(6);
    }
    embk_puts(1, "compositor: S-surf-1 (cross-addr-space over real IPC) PASS\n");

    /* ---- S-surf-2: ownership rejects a commit the compositor doesn't own ---- */
    if (embk_surface_commit(s1, 0) >= 0) { embk_puts(1, "compositor: S-surf-2 FAIL\n"); embk_exit(7); }
    int other = embk_surface_acquire(s1);
    if (other != 1) { embk_puts(1, "compositor: S-surf-2 acquire-other FAIL\n"); embk_exit(7); }
    embk_puts(1, "compositor: S-surf-2 (ownership) PASS\n");

    /* ---- S-surf-3: starve both buffers, release, reacquire ---- */
    if (embk_surface_commit(s1, other) != 0) { embk_puts(1, "compositor: S-surf-3 commit FAIL\n"); embk_exit(8); }
    if (embk_surface_acquire(s1) >= 0) { embk_puts(1, "compositor: S-surf-3 not-starved FAIL\n"); embk_exit(8); }
    if (embk_surface_release(s1, 0) != 0) { embk_puts(1, "compositor: S-surf-3 release FAIL\n"); embk_exit(8); }
    if (embk_surface_acquire(s1) != 0) { embk_puts(1, "compositor: S-surf-3 reacquire FAIL\n"); embk_exit(8); }
    embk_puts(1, "compositor: S-surf-3 (starvation/release) PASS\n");

    /* ---- "move": the MOVE'd surface arrives correctly too ---- */
    len = 0; nh = 0;
    if (embk_chan_recv(ch, buf, sizeof(buf), &len, hnds, &nh) < 0 || nh != 1) {
        embk_puts(1, "compositor: recv(move) FAIL\n"); embk_exit(9);
    }
    int s2 = hnds[0];
    long base2 = embk_surface_map(s2, &info);
    if (base2 < 0 || ((volatile unsigned *)base2)[0] != SURF_PATTERN_2) {
        embk_puts(1, "compositor: S-move receiver-side FAIL\n"); embk_exit(10);
    }
    embk_puts(1, "compositor: S-move (receiver side) PASS\n");

    long client_code = embk_wait((int)spawn_h);
    if (client_code != 66) { embk_puts(1, "compositor: client exit code FAIL\n"); embk_exit(11); }

    embk_exit(88);   /* deliberately no chan_close/surface_destroy -- R2 does it all */
}

/* ---- B4: a listener that registers, then exits WITHOUT ever accepting or
 * closing -- simulating a crash. R2 process-exit cleanup must unregister its
 * epfs node so a later connect() is refused cleanly, not left dangling
 * (unlike a stale Unix socket file). */
static void b4_listen_role(void)
{
    int lh = embk_chan_listen("/run/b4test");
    embk_exit(lh < 0 ? 1 : 0);   /* exits immediately -- no accept, no close */
}

/* Attempts to connect to a path whose owner has already exited+been reaped.
 * Must be refused (ENOENT or ECONNREFUSED), never succeed, never hang. */
static void b4_connect_role(void)
{
    int rc = embk_chan_connect("/run/b4test");
    /* Encode the outcome so the selftest can assert the SPECIFIC failure
     * (a gone node -> vfs_resolve returns -ENOENT), not just "rc<0". An
     * earlier "any rc<0 -> 42" masked a get_endpoint wiring bug that made
     * connect fail with ENOSYS on a LIVE endpoint -- never again. */
    embk_exit(rc < 0 ? -rc : 255);   /* 255 = wrongly succeeded */
}

/* ========================================================================= */
/* EmbLink UI Piece 2: the compositor PROTOCOL (message vocabulary over Piece */
/* 1 channels). These roles are the proving selftests P2-S1..S5. Each runs    */
/* single-process: a chan_pair (or a self-connect through /run rendezvous for  */
/* the privilege test) gives us BOTH the client end and the compositor end,    */
/* and one thread drives each side in turn -- deterministic, no scheduling     */
/* races, every message-tagging invariant still exercised over the real        */
/* channel syscalls. Exit 0 == scenario passed; nonzero == failing step code.  */
/* ========================================================================= */

#include "embk_ui_proto.h"

/* Send header+payload as one message. Returns 0 or -EMBK_*. */
static int ui_send(int ch, uint16_t type, uint32_t req, uint32_t win,
                   const void *pl, uint32_t pl_len)
{
    unsigned char buf[EMBK_UI_MSG_MAX];
    unsigned len = embk_ui_pack(buf, type, req, win, pl, pl_len);
    return embk_chan_send(ch, buf, len, 0, 0, 0);
}

/* Send header (no payload) carrying exactly one ancillary handle. */
static int ui_send_h(int ch, uint16_t type, uint32_t req, uint32_t win, int hnd, int flag)
{
    unsigned char buf[EMBK_UI_MSG_MAX];
    unsigned len = embk_ui_pack(buf, type, req, win, 0, 0);
    return embk_chan_send(ch, buf, len, &hnd, &flag, 1);
}

/* Receive one message into `buf`; validate the envelope (P sanity check).
 * Returns the header on success (cast of buf), or 0 on channel error/malformed.
 * *out_total gets chan_recv's authoritative length; got_hnd (if non-NULL) gets
 * the single ancillary handle or -1 if none. */
static struct msg_header *ui_recv(int ch, void *buf, unsigned *out_total, int *got_hnd)
{
    unsigned total = 0, nh = 0; int hnds[4];
    int rc = embk_chan_recv(ch, buf, EMBK_UI_MSG_MAX, &total, hnds, &nh);
    if (rc < 0) { if (got_hnd) *got_hnd = -1; return 0; }
    if (!embk_ui_check(buf, total)) { if (got_hnd) *got_hnd = -1; return 0; }
    if (out_total) *out_total = total;
    if (got_hnd) *got_hnd = (nh >= 1) ? hnds[0] : -1;
    return (struct msg_header *)buf;
}

/* The crux of async/tagged dispatch (Section 1): block-recv on `ch`, treating
 * every message whose request_id != want_req as a pure event to dispatch and
 * skip, until the reply echoing want_req lands -- from wherever in the stream.
 * *events_before gets how many events were skipped first. Returns the reply
 * header (in `buf`), or 0 on error. */
static struct msg_header *ui_await_reply(int ch, uint32_t want_req, void *buf,
                                         unsigned *out_total, int *events_before)
{
    int n = 0;
    for (;;) {
        struct msg_header *h = ui_recv(ch, buf, out_total, 0);
        if (!h) return 0;
        if (h->request_id == want_req) { if (events_before) *events_before = n; return h; }
        n++;   /* request_id==0 pure event (or unrelated reply): dispatch & keep looping */
    }
}

/* ---- P2-S1: handshake + correlation (Section 3, invariants P1/P3) -------- */
static int ui_scen_handshake(void)
{
    int h[2];
    if (embk_chan_pair(h) < 0) return 1;
    unsigned char cb[EMBK_UI_MSG_MAX], sb[EMBK_UI_MSG_MAX];
    unsigned total;

    /* --- good version: HELLO(v=1, req=7) -> HELLO_ACK(req=7, status=0) --- */
    struct msg_hello hello = { EMBK_UI_PROTOCOL_VERSION };
    if (ui_send(h[0], MSG_HELLO, 7, 0, &hello, sizeof(hello)) < 0) return 2;

    struct msg_header *rq = ui_recv(h[1], sb, &total, 0);
    if (!rq || rq->type != MSG_HELLO || rq->request_id != 7) return 3;
    const struct msg_hello *hp = (const struct msg_hello *)embk_ui_payload(sb);
    struct msg_hello_ack ack;
    ack.compositor_version = EMBK_UI_PROTOCOL_VERSION;
    ack.status = (hp->client_version == EMBK_UI_PROTOCOL_VERSION) ? 0 : -EMBK_EPROTO;
    if (ui_send(h[1], MSG_HELLO_ACK, rq->request_id, 0, &ack, sizeof(ack)) < 0) return 4;

    int ev = -1;
    struct msg_header *rp = ui_await_reply(h[0], 7, cb, &total, &ev);
    if (!rp || rp->type != MSG_HELLO_ACK) return 5;
    if (ev != 0) return 6;   /* no events should have preceded this reply */
    const struct msg_hello_ack *ap = (const struct msg_hello_ack *)embk_ui_payload(cb);
    if (ap->status != 0) return 7;

    /* --- bad version: HELLO(v=99, req=8) -> HELLO_ACK(-EPROTO) then CLOSE (P3) --- */
    struct msg_hello bad = { 99 };
    if (ui_send(h[0], MSG_HELLO, 8, 0, &bad, sizeof(bad)) < 0) return 8;
    rq = ui_recv(h[1], sb, &total, 0);
    if (!rq || rq->type != MSG_HELLO || rq->request_id != 8) return 9;
    hp = (const struct msg_hello *)embk_ui_payload(sb);
    ack.status = (hp->client_version == EMBK_UI_PROTOCOL_VERSION) ? 0 : -EMBK_EPROTO;
    if (ack.status != -EMBK_EPROTO) return 10;
    if (ui_send(h[1], MSG_HELLO_ACK, 8, 0, &ack, sizeof(ack)) < 0) return 11;
    embk_chan_close(h[1]);   /* P3: refuse then close */

    /* Client still receives the queued ACK... */
    rp = ui_await_reply(h[0], 8, cb, &total, &ev);
    if (!rp || rp->type != MSG_HELLO_ACK) return 12;
    ap = (const struct msg_hello_ack *)embk_ui_payload(cb);
    if (ap->status != -EMBK_EPROTO) return 13;
    /* ...then its NEXT recv sees the closed peer (A4/P3). */
    unsigned t2 = 0, nh = 0; int hnds[4];
    int rc = embk_chan_recv(h[0], cb, sizeof(cb), &t2, hnds, &nh);
    if (rc != -EMBK_EPIPE) return 14;

    embk_chan_close(h[0]);
    return 0;
}

/* ---- P2-S3: out-of-order reply survives (Section 9, the crux) ------------ */
static int ui_scen_reorder(void)
{
    int h[2];
    if (embk_chan_pair(h) < 0) return 1;
    unsigned char cb[EMBK_UI_MSG_MAX], sb[EMBK_UI_MSG_MAX];
    unsigned total;

    /* client -> CREATE_SURFACE_ROLE(req=5) */
    struct msg_create_surface_role req = { ROLE_APP_WINDOW, 320, 240 };
    if (ui_send(h[0], MSG_CREATE_SURFACE_ROLE, 5, 0, &req, sizeof(req)) < 0) return 2;

    /* compositor receives it, then ENQUEUES an unrelated event AHEAD of the
     * reply, then the reply -- exactly the hazard async/tagged dispatch exists
     * to survive. */
    struct msg_header *rq = ui_recv(h[1], sb, &total, 0);
    if (!rq || rq->type != MSG_CREATE_SURFACE_ROLE || rq->request_id != 5) return 3;
    struct msg_pointer_motion mot = { 12, 34 };
    if (ui_send(h[1], MSG_POINTER_MOTION, 0, 1, &mot, sizeof(mot)) < 0) return 4;  /* event, id=0 */
    struct msg_role_created created = { 0, 1, 320, 240 };
    if (ui_send(h[1], MSG_ROLE_CREATED, 5, 1, &created, sizeof(created)) < 0) return 5;  /* reply */

    /* client dispatch loop: the motion event MUST arrive & be dispatched
     * first (request_id 0), and the reply matched by id 5 -- NOT the naive
     * send();recv() that would misread the motion AS the reply. */
    int ev = -1;
    struct msg_header *rp = ui_await_reply(h[0], 5, cb, &total, &ev);
    if (!rp || rp->type != MSG_ROLE_CREATED) return 6;
    if (ev < 1) return 7;   /* proves >=1 event really jumped ahead of the reply */
    const struct msg_role_created *cp = (const struct msg_role_created *)embk_ui_payload(cb);
    if (cp->status != 0 || cp->window_id != 1) return 8;

    embk_chan_close(h[0]); embk_chan_close(h[1]);
    return 0;
}

/* ---- P2-S2: privilege is STRUCTURAL, follows the endpoint (P4) ----------- */
/* Runs a HELLO + CREATE_SURFACE_ROLE(BACKGROUND) exchange over a channel of a
 * given provenance; `trusted` is whether the channel was accepted on the
 * shell endpoint. Returns the ROLE_CREATED status the compositor produced. */
static int ui_priv_exchange(int cli, int srv, int trusted, int32_t *out_status)
{
    unsigned char cb[EMBK_UI_MSG_MAX], sb[EMBK_UI_MSG_MAX];
    unsigned total;
    struct msg_create_surface_role req = { ROLE_BACKGROUND, 0, 0 };
    if (ui_send(cli, MSG_CREATE_SURFACE_ROLE, 9, 0, &req, sizeof(req)) < 0) return 1;

    struct msg_header *rq = ui_recv(srv, sb, &total, 0);
    if (!rq || rq->type != MSG_CREATE_SURFACE_ROLE) return 2;
    const struct msg_create_surface_role *rp = (const struct msg_create_surface_role *)embk_ui_payload(sb);

    /* The compositor validates the requested role against the channel's
     * PROVENANCE (which listener it came in on), never the client's claim:
     * a privileged role on an untrusted channel is -EACCES. */
    struct msg_role_created created = { 0, 0, 0, 0 };
    int privileged = (rp->role == ROLE_BACKGROUND || rp->role == ROLE_PANEL);
    if (privileged && !trusted) { created.status = -EMBK_EACCES; created.window_id = 0; }
    else                        { created.status = 0;            created.window_id = 1; }
    if (ui_send(srv, MSG_ROLE_CREATED, rq->request_id, 0, &created, sizeof(created)) < 0) return 3;

    int ev = -1;
    struct msg_header *ar = ui_await_reply(cli, 9, cb, &total, &ev);
    if (!ar || ar->type != MSG_ROLE_CREATED) return 4;
    const struct msg_role_created *ap = (const struct msg_role_created *)embk_ui_payload(cb);
    *out_status = ap->status;
    return 0;
}

static int ui_scen_privilege(void)
{
    /* Two real listening endpoints (Piece 1 Layer B): one privileged. A
     * process can connect to its OWN endpoint, so we exercise the actual
     * accept path -- provenance = which listen handle accept() returned. */
    int lh_app   = embk_chan_listen("/run/compositor");
    if (lh_app < 0) return 1;
    int lh_shell = embk_chan_listen("/run/compositor-shell");
    if (lh_shell < 0) return 2;

    int32_t st_untrusted = 123, st_trusted = 123;

    /* untrusted: connect via /run/compositor, request BACKGROUND -> EACCES */
    int c1 = embk_chan_connect("/run/compositor");
    if (c1 < 0) return 3;
    int s1 = embk_chan_accept(lh_app);      /* provenance: app endpoint = untrusted */
    if (s1 < 0) return 4;
    if (ui_priv_exchange(c1, s1, 0, &st_untrusted) != 0) return 5;
    if (st_untrusted != -EMBK_EACCES) return 6;
    embk_chan_close(c1); embk_chan_close(s1);

    /* trusted: connect via /run/compositor-shell, request BACKGROUND -> OK */
    int c2 = embk_chan_connect("/run/compositor-shell");
    if (c2 < 0) return 7;
    int s2 = embk_chan_accept(lh_shell);    /* provenance: shell endpoint = trusted */
    if (s2 < 0) return 8;
    if (ui_priv_exchange(c2, s2, 1, &st_trusted) != 0) return 9;
    if (st_trusted != 0) return 10;
    embk_chan_close(c2); embk_chan_close(s2);

    /* endpoints unregister via R2 on exit; close handles now. */
    embk_chan_close(lh_app); embk_chan_close(lh_shell);
    return 0;
}

/* ---- P2-S4: frame pacing honesty -- FRAME_DONE only after release (P5) --- */
static int ui_scen_pacing(void)
{
    int h[2];
    if (embk_chan_pair(h) < 0) return 1;
    unsigned char cb[EMBK_UI_MSG_MAX], sb[EMBK_UI_MSG_MAX];
    unsigned total;

    /* Minimum is 2 buffers (double-buffering). Commit BOTH so NO buffer is
     * free until the compositor actually surface_release()s one -- then "can
     * the client acquire again?" is a direct, observable proxy for "did
     * release happen?". */
    struct embk_surface_info info;
    int s_cli = embk_surface_create(4, 4, EMBK_PIXFMT_BGRA8888_PRE, 2, &info);
    if (s_cli < 0) return 2;
    long base = embk_surface_map(s_cli, &info);
    if (base < 0) return 4;
    int idx0 = embk_surface_acquire(s_cli);
    if (idx0 < 0) return 3;
    ((volatile unsigned *)base)[0] = SURF_PATTERN_1;
    if (embk_surface_commit(s_cli, idx0) != 0) return 5;
    int idx1 = embk_surface_acquire(s_cli);
    if (idx1 < 0) return 3;
    if (embk_surface_commit(s_cli, idx1) != 0) return 5;
    /* both buffers now compositor-owned: a re-acquire MUST starve */
    if (embk_surface_acquire(s_cli) != -EMBK_EAGAIN) return 6;

    /* client attaches the surface (COPY) + commits the frame it drew (idx0) */
    if (ui_send_h(h[0], MSG_SURFACE_ATTACH, 0, 1, s_cli, EMBK_CHAN_HANDLE_COPY) < 0) return 7;
    struct msg_frame_commit fc = { (uint32_t)idx0, 0, 0, 0, 0 };
    if (ui_send(h[0], MSG_FRAME_COMMIT, 0, 1, &fc, sizeof(fc)) < 0) return 8;

    /* compositor receives the surface handle, then the commit */
    int s_comp = -1;
    struct msg_header *am = ui_recv(h[1], sb, &total, &s_comp);
    if (!am || am->type != MSG_SURFACE_ATTACH || s_comp < 0) return 9;
    struct msg_header *fm = ui_recv(h[1], sb, &total, 0);
    if (!fm || fm->type != MSG_FRAME_COMMIT) return 10;
    uint32_t committed_idx = ((const struct msg_frame_commit *)embk_ui_payload(sb))->buffer_idx;
    /* it's genuinely holding the frame -- acquire on ITS handle also starves */
    if (embk_surface_acquire(s_comp) != -EMBK_EAGAIN) return 11;

    /* P5: release FIRST, THEN signal done. (Order is by construction here;
     * the test proves the OBSERVABLE contract below.) */
    if (embk_surface_release(s_comp, (int)committed_idx) != 0) return 12;
    if (ui_send(h[1], MSG_FRAME_DONE, 0, 1, 0, 0) < 0) return 13;

    /* client waits for FRAME_DONE (a pure event), then the buffer MUST be
     * re-acquirable -- if the compositor had sent DONE before releasing, this
     * acquire would still be -EAGAIN. That's the P5 violation this catches. */
    for (;;) {
        struct msg_header *dm = ui_recv(h[0], cb, &total, 0);
        if (!dm) return 14;
        if (dm->type == MSG_FRAME_DONE) break;
    }
    if (embk_surface_acquire(s_cli) != 0) return 15;   /* proof: release really happened */

    embk_surface_destroy(s_cli);
    embk_chan_close(h[0]); embk_chan_close(h[1]);
    return 0;
}

/* ---- P2-S5: input routing is per-window, never broadcast (Section 9) ----- */
static int ui_scen_routing(void)
{
    int a[2], b[2];
    if (embk_chan_pair(a) < 0) return 1;   /* a[0]=client A, a[1]=compositor->A */
    if (embk_chan_pair(b) < 0) return 2;   /* b[0]=client B, b[1]=compositor->B */
    unsigned char buf[EMBK_UI_MSG_MAX];
    unsigned total;

    /* Compositor synthesizes a pointer over window A: input goes to A ONLY. */
    struct msg_pointer_enter  en = { 5, 5 };
    struct msg_pointer_motion mo = { 6, 7 };
    if (ui_send(a[1], MSG_POINTER_ENTER, 0, 1, &en, sizeof(en)) < 0) return 3;
    if (ui_send(a[1], MSG_POINTER_MOTION, 0, 1, &mo, sizeof(mo)) < 0) return 4;
    /* Then a sync marker (a CONFIGURE) on BOTH, so each client has something
     * to receive without needing a nonblocking recv. */
    struct msg_configure cfg = { 320, 240, WIN_STATE_ACTIVATED };
    if (ui_send(a[1], MSG_CONFIGURE, 0, 1, &cfg, sizeof(cfg)) < 0) return 5;
    if (ui_send(b[1], MSG_CONFIGURE, 0, 2, &cfg, sizeof(cfg)) < 0) return 6;

    /* Client A must see: ENTER, MOTION, then the CONFIGURE sync. */
    struct msg_header *m = ui_recv(a[0], buf, &total, 0);
    if (!m || m->type != MSG_POINTER_ENTER) return 7;
    m = ui_recv(a[0], buf, &total, 0);
    if (!m || m->type != MSG_POINTER_MOTION) return 8;
    m = ui_recv(a[0], buf, &total, 0);
    if (!m || m->type != MSG_CONFIGURE) return 9;

    /* Client B's FIRST message must be the CONFIGURE sync -- NO pointer event
     * ahead of it. A broadcast would have delivered ENTER/MOTION to B too. */
    m = ui_recv(b[0], buf, &total, 0);
    if (!m || m->type != MSG_CONFIGURE) return 10;

    embk_chan_close(a[0]); embk_chan_close(a[1]);
    embk_chan_close(b[0]); embk_chan_close(b[1]);
    return 0;
}

static void ui_proto_role(const char *scen)
{
    int rc;
    if      (embk_streq(scen, "hs"))        rc = ui_scen_handshake();
    else if (embk_streq(scen, "reorder"))   rc = ui_scen_reorder();
    else if (embk_streq(scen, "privilege")) rc = ui_scen_privilege();
    else if (embk_streq(scen, "pacing"))    rc = ui_scen_pacing();
    else if (embk_streq(scen, "routing"))   rc = ui_scen_routing();
    else rc = 200;
    embk_exit(rc);   /* 0 == scenario passed */
}

/* rdi=argc, rsi=argv -- delivered by the kernel's process_trampoline has_argv
 * path. No crt0: these C-ABI parameter registers ARE where the trampoline
 * put them. */
void _start(long argc, char **argv)
{
    /* Spawned-child role: do exactly what spawn_test() above checks, then
     * exit. Exiting HERE (not falling through) is what stops init from
     * recursively spawning itself without bound. */
    if (argc >= 2) {
        /* EmbLink UI Piece 1 roles, selected by argv[1]. Each exits. */
        if (embk_streq(argv[1], "surface-parent")) surface_parent();
        if (embk_streq(argv[1], "surface-child"))  surface_child(argc, argv);
        if (embk_streq(argv[1], "chan"))           chan_role();
        if (embk_streq(argv[1], "compositor"))        compositor_role();
        if (embk_streq(argv[1], "compositor-client"))  compositor_client_role();
        if (embk_streq(argv[1], "b4-listen"))          b4_listen_role();
        if (embk_streq(argv[1], "b4-connect"))         b4_connect_role();
        /* EmbLink UI Piece 2 protocol scenarios; argv[2] selects which. */
        if (embk_streq(argv[1], "ui-proto") && argc >= 3) ui_proto_role(argv[2]);

        /* Long-lived child for the handle-reap selftest: never exits on its own
         * (the test kills it). Lets the test prove process_handle_reap_dead()
         * reclaims DEAD children while leaving a LIVE one's handle alone. */
        if (embk_streq(argv[1], "spin")) { for (;;) embk_sleep_ms(1000); }

        /* Default spawned-child role (spawn_test echo): exit HERE, never fall
         * through into the suite (would recursively spawn forever). */
        embk_write(3, argv[1], embk_strlen(argv[1]));   /* fd 3: parent-redirected */
        embk_exit((int)argc);                            /* parent asserts == 2 */
    }

    /* Top-level role: run the whole native-primitive suite, exit 16 iff all
     * passed (the sentinel `test ring3 threads` checks). */
    int ok = 1;
    ok &= thread_test();
    ok &= spawn_test();
    ok &= sbrk_test();
    embk_exit(ok ? 16 : 1);
}
