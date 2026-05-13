target remote localhost:1234
symbol-file kernel/kernel.elf
set disassembly-flavor intel
break kernel_main
continue