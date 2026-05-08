ENTRY(kernel_main)

SECTIONS {
    . = 0x100000; /* Start of the kernel in memory at 1MB */

    .text ALIGN(4096) : {
        *(.text) /* All .text sections from input files */
    }

    .rodata ALIGN(4096) : {
        *(.rodata) /* All .rodata sections from input files */
    }

    .data ALIGN(4096) : {
        *(.data) /* All .data sections from input files */
    }

    .bss ALIGN(4096) : {
    *(.bss)
    *(COMMON)
}
}