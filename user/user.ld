/* user/user.ld — force separate R-X and R-W LOAD segments (real W^X) */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* 5 = R(4) | X(1)  -> read + execute, NO write */
    data PT_LOAD FLAGS(6);   /* 6 = R(4) | W(2)  -> read + write,  NO execute */
}

SECTIONS
{
    . = 0x400000;

    .text   : { *(.text.start) *(.text .text.*) }   :text
    .rodata : { *(.rodata .rodata.*) }               :text

    . = ALIGN(0x1000);                               /* new page for writable data */

    .data   : { *(.data .data.*) }                   :data
    .bss    : { *(.bss  .bss.*)  }                    :data

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}