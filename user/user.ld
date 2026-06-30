/*  place the program at the fixed user base, _start first. */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;                 /* THE fixed user base — must match the loader */

    .text : {
        *(.text.start)            /* _start goes first, so entry == 0x400000 */
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }   /* the string lives here */
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) }

    /* Drop anything we don't want in a flat binary. */
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}