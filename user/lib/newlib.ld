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

    .data : {
        *(.data .data.* .gnu.linkonce.d.*)
    } :data

    /* Static constructor/destructor arrays. crt0 does NOT invoke these yet
     * (no __libc_init_array call -- pure-C programs don't need them), but
     * they're placed correctly so adding that later is a crt0-only change. */
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
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
