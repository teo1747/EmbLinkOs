ENTRY(kernel_main)


KERNEL_VIRTUAL_BASE = 0XFFFFFFFF80000000;
KERNEL_PHYSICAL_BASE = 0X100000;
BOOT_STACK_TOP_PHYS = 0X200000;


SECTIONS {
    . = KERNEL_VIRTUAL_BASE + KERNEL_PHYSICAL_BASE; /* Start of the kernel in memory at 1MB */

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VIRTUAL_BASE) {
        *(.text) /* All .text sections from input files */
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VIRTUAL_BASE) {
        *(.rodata) /* All .rodata sections from input files */
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VIRTUAL_BASE){
        *(.data) /* All .data sections from input files */
    }

    .bss ALIGN(4096) : AT(ADDR(.data) - KERNEL_VIRTUAL_BASE){
    *(.bss)
    *(COMMON)
}

    /* Mark the end of the kernel in memory */
    . = ALIGN(4096);
    kernel_end = .;

}