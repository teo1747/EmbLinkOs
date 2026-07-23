/* emlink_dynstubs.s -- the tcc-world equivalent of newlib.ld's PROVIDE()s.
   TCC links with no linker script, so crt0's WEAK bracket symbols have no
   definition, and TCC (unlike gnu ld) turns a weak-undefined symbol into a
   DYNAMIC IMPORT the in-kernel loader cannot resolve. Define them as WEAK
   ABSOLUTE 0 -- the correct geometry for a C app with no static ctors and no
   __thread TLS, which is exactly what TCC can build. Weak: a real definition
   (a genuine .ctors, a TLS-bearing newlib.ld) still wins. */
.weak __ctors_start
.weak __ctors_end
.weak __tls_image
.weak __tls_filesz
.weak __tls_memsz
.weak __tls_align
.set __ctors_start, 0
.set __ctors_end,   0
.set __tls_image,   0
.set __tls_filesz,  0
.set __tls_memsz,   0
.set __tls_align,   0
