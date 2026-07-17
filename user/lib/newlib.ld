/* user/newlib.ld — link script for newlib-linked EmbLink programs (crt0.c +
 * syscalls.c + the program + libc.a). Same 0x400000 load address and W^X
 * two-segment split as user.ld, but with the sections a full C library
 * actually emits: .rodata (string literals, format strings), the C++/ctor
 * arrays (kept so the layout is honest even though crt0 doesn't run them yet
 * -- see crt0.c), and a real .bss the kernel's ELF loader zero-fills. */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R + X, no write */
    data PT_LOAD FLAGS(6);   /* R + W, no execute */
    /* PT_TLS describes the TLS TEMPLATE. It is not separately loaded: it covers
     * the same bytes as the data segment, and each thread gets its own COPY made
     * at runtime (crt0). Declaring it is mandatory here -- once a script has an
     * explicit PHDRS block, ld emits ONLY the program headers it names, so
     * without this line .tdata/.tbss become orphan sections with no PT_TLS and
     * every %fs access faults. */
    tls  PT_TLS  FLAGS(4);   /* R */
}

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text.start)
        *(.text .text.* .gnu.linkonce.t.*)
    } :text

    .rodata : {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
    } :text

    . = ALIGN(0x1000);       /* new page: writable data below, W^X boundary */

    /* --- Thread-local storage ------------------------------------------------
     * .tdata is the initialised TLS image (real bytes in the file); .tbss is the
     * zero-filled remainder (NOBITS -- it occupies no file space and, for TLS,
     * no address space either: whatever section follows starts at .tbss's own
     * address). Both belong to :data AND :tls -- the data segment carries the
     * template bytes, PT_TLS describes them.
     *
     * crt0 needs the geometry to build a thread's block, and it cannot read the
     * program headers (no auxv on this OS), so the linker hands it over through
     * these symbols. SIZEOF()/ALIGNOF() are used deliberately instead of
     * differencing the location counter around .tbss: `.` does NOT advance
     * across a NOBITS TLS section, so a `__tls_end - __tls_start` idiom would
     * silently compute a memsz that omits .tbss entirely -- and .tbss is exactly
     * where CPython's _Py_tss_tstate lives. */
    .tdata : {
        __tls_image = .;
        *(.tdata .tdata.* .gnu.linkonce.td.*)
    } :data :tls

    .tbss : {
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)
    } :data :tls

    /* Bytes to COPY from __tls_image (the .tdata image) ... */
    __tls_filesz = SIZEOF(.tdata);
    /* ... and the full block size; the tail (memsz - filesz) is zeroed. This
     * must equal PT_TLS's p_memsz EXACTLY -- crt0 derives every variable's
     * address from it (TP - ALIGN(memsz, align) + offset).
     *
     * SPAN it (end of .tbss minus start of .tdata) rather than SIZEOF(.tdata) +
     * SIZEOF(.tbss): the linker inserts ALIGNMENT PADDING between the two
     * sections whenever a TLS variable is over-aligned, and the sum silently
     * omits that gap. Measured with one 32-byte-aligned variable: the real
     * p_memsz was 0x2c while the sum said 0x10 -- crt0 would then have rounded
     * to 0x20 instead of 0x40 and placed EVERY thread-local 32 bytes off, with
     * no fault to show for it. */
    __tls_memsz  = ADDR(.tbss) + SIZEOF(.tbss) - ADDR(.tdata);
    /* PT_TLS's p_align. crt0 MUST round memsz up with the same value the linker
     * used, because a variable's address is TP - ALIGN(memsz, align) + offset:
     * disagree here and every TLS variable lands at the wrong address. MAX()
     * mirrors how the phdr's alignment is derived from its sections; the 8 floor
     * covers the degenerate case where a section is empty and reports align 1. */
    __tls_align  = MAX(MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)), 8);

    .data : {
        *(.data .data.* .gnu.linkonce.d.*)
    } :data

    /* Static constructors. crt0's run_init_array()/run_ctors() walk these
     * before main -- C++ global constructors and C's
     * __attribute__((constructor)) both land here.
     *
     * BOTH schemes are collected on purpose. GCC picks between them at ITS
     * configure time (--enable-initfini-array): the stock /usr/local/cross
     * C compiler was built --without-headers for bare metal and emits the
     * LEGACY .ctors, while a newlib-aware build emits the modern
     * .init_array. Collecting only .init_array is why a constructor silently
     * never ran -- the entry sat in an uncollected .ctors and the
     * __init_array brackets came out empty (start == end). Taking both means
     * constructors work whichever toolchain compiled the object. */
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :data

    /* Legacy GNU scheme. No crtbegin/crtend here (-nostartfiles), so there
     * are no -1/0 sentinels -- just the raw entries; crt0 walks them
     * BACKWARD, which is the order this format expects. */
    .ctors : {
        __ctors_start = .;
        KEEP(*(SORT(.ctors.*)))
        KEEP(*(.ctors))
        __ctors_end = .;
    } :data

    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
        __bss_end = .;
    } :data

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.eh_frame_hdr) }
}
