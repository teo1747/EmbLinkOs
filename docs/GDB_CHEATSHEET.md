# GDB Cheat Sheet for EmbLinkOs Kernel Debugging

## Getting Started

### Terminal 1 — Start QEMU paused
```bash
make debug
```
QEMU starts with the CPU frozen, waiting for GDB to attach.

### Terminal 2 — Connect GDB
```bash
cd ~/myos
gdb
```
The `.gdbinit` file auto-connects to QEMU and stops at `kernel_main`.

---

## Essential Commands

### Code Navigation
| Command | Description |
|---|---|
| `list` (or `l`) | Show 10 lines around current position |
| `list function_name` | Show source of a function |
| `list filename:line` | Show specific file/line |
| `list -` | Show lines before current |

### Execution Control
| Command | Description |
|---|---|
| `next` (or `n`) | Execute next line, step OVER function calls |
| `step` (or `s`) | Execute next line, step INTO function calls |
| `continue` (or `c`) | Run until next breakpoint or end |
| `finish` | Run until current function returns |
| `until line` | Run until specified line |
| `stepi` (or `si`) | Step one CPU instruction |
| `nexti` (or `ni`) | Step one instruction, over calls |

### Breakpoints
| Command | Description |
|---|---|
| `break function_name` | Break when entering function |
| `break file.c:42` | Break at file line 42 |
| `break *0x10012b` | Break at raw address |
| `info breakpoints` | List all breakpoints |
| `delete N` | Delete breakpoint N |
| `disable N` | Disable breakpoint N (keep it) |
| `enable N` | Re-enable breakpoint N |
| `clear` | Delete breakpoint at current line |

### Watchpoints (break when variable changes)
| Command | Description |
|---|---|
| `watch variable` | Break when variable is written |
| `rwatch variable` | Break when variable is read |
| `awatch variable` | Break on read OR write |

---

## Inspecting State

### Registers
| Command | Description |
|---|---|
| `info registers` | Show all CPU registers |
| `info registers rax rbx` | Show specific registers |
| `print $rax` | Print one register value |
| `print/x $rsp` | Print in hexadecimal |
| `print/t $rflags` | Print in binary |

### Variables
| Command | Description |
|---|---|
| `print var` | Show variable value |
| `print/x var` | Show in hex |
| `print *ptr` | Dereference pointer |
| `print array[3]` | Show array element |
| `print struct_var.field` | Show struct field |
| `display var` | Show variable every step |
| `undisplay N` | Stop showing N |

### Memory
| Command | Description |
|---|---|
| `x/16xb 0x7000` | 16 bytes in hex |
| `x/16xw 0x7000` | 16 words (4 bytes each) |
| `x/4xg 0x7000` | 4 giant words (8 bytes each) |
| `x/s 0x10020` | Read as null-terminated string |
| `x/16i $rip` | 16 instructions at RIP |

**Format codes:** `x` (hex), `d` (decimal), `u` (unsigned), `s` (string), `i` (instruction), `c` (char), `f` (float), `t` (binary)
**Size codes:** `b` (byte), `h` (halfword 2B), `w` (word 4B), `g` (giant 8B)

---

## Stack Inspection

| Command | Description |
|---|---|
| `backtrace` (or `bt`) | Show call stack |
| `frame N` | Switch to frame N |
| `up` | Go up one frame (caller) |
| `down` | Go down one frame |
| `info args` | Show function arguments |
| `info locals` | Show local variables |

---

## EmbLinkOs-Specific Debugging

### Check the E820 Memory Map
```gdb
print *(uint32_t*)0x7000              ; entry count
x/6xw 0x7004                          ; first entry
x/24xb 0x7004                         ; first entry as bytes
```

### Verify the Kernel Was Loaded
```gdb
x/4xb 0x10000                         ; should show 7F 45 4C 46 (ELF magic)
x/s 0x100400                          ; might show some kernel strings
```

### Check the PMM Bitmap
```gdb
print total_pages
print free_pages
print used_pages
print kernel_end
x/16xb pmm_bitmap                     ; first 16 bytes of bitmap
```

### Inspect the IDT
```gdb
print idt_ptr.base
print idt_ptr.limit
x/16xb idt                            ; first IDT entry (16 bytes)
```

### Check CPU State
```gdb
print $cr0                            ; control register 0
print $cr3                            ; page table base
print $cr4                            ; control register 4
info registers                        ; everything
```

### Catch An Exception
```gdb
break isr_handler
continue
; when triggered:
print regs->vector
print regs->error_code
print/x regs->rip
```

---

## Common Workflows

### Trace a function
```gdb
break pmm_init
continue
; now stepping through
next
next
print free_pages
```

### Find what's at a crash address
```gdb
; if RIP shows 0x100250 when crashed
list *0x100250
disassemble 0x100250
```

### Watch a variable change
```gdb
break kernel_main
continue
watch free_pages
continue
; breaks every time free_pages is modified
```

### Examine raw memory after suspected corruption
```gdb
x/64xb 0x300000                       ; dump 64 bytes
x/16xg 0x300000                       ; same but as 64-bit values
```

---

## Useful Display Settings

```gdb
set disassembly-flavor intel          ; Intel syntax (default in our .gdbinit)
set print pretty on                   ; format structs nicely
set print array on                    ; print arrays one element per line
layout src                            ; show source code panel
layout asm                            ; show assembly panel
layout regs                           ; show registers panel
Ctrl+X then 1                         ; back to single window
```

---

## Quitting

| Command | Description |
|---|---|
| `quit` (or `q`) | Exit GDB |
| `kill` | Kill the program but stay in GDB |

When you `quit`, QEMU also exits since we used `-S -s` flags.

---

## Troubleshooting

### "No symbol table" error
Make sure CFLAGS in Makefile includes `-g`:

CFLAGS = -ffreestanding -nostdlib -nostartfiles 
-mno-red-zone -mno-mmx -mno-sse -mno-sse2 
-g -O0


### GDB blocks auto-loading .gdbinit
Run once:
```bash
mkdir -p ~/.config/gdb
echo "add-auto-load-safe-path /home/motsou/myos/.gdbinit" >> ~/.config/gdb/gdbinit
```

### Want to debug Stage 1/2 (bootloader)
Stage 1 and Stage 2 are raw binaries with no symbols. Use:
```gdb
break *0x7C00          ; Stage 1 entry
break *0x7E00          ; Stage 2 entry
stepi                  ; step one instruction at a time
x/10i $pc              ; show next 10 instructions
info registers
```
## Diagnosing a "hang" without GDB: the QEMU monitor RIP dump

Often faster than a full GDB session, and it works on an already-running VM.
Launch with a monitor socket:

```bash
qemu-system-x86_64 ... -monitor unix:/tmp/mon.sock,server,nowait
```

When the machine goes quiet:

```bash
printf "info registers\n" | nc -U /tmp/mon.sock | grep -E "RIP|RFL|HLT"
x86_64-elf-addr2line -e kernel/kernel.elf -f 0x<RIP>
```

One command turns "silent freeze" into a file:line. Read `RFL` too:
`RFL=00000002` means **IF=0** — combined with `HLT=1` that is a permanently
sleeping CPU (this exact readout pinned ledger Bug 26, the IF=0
voluntary-block wake leak, at `ata_wait_irq`'s `hlt`). Sample twice a few
seconds apart: a *moving* RIP means the machine is alive and something
higher-level is stuck (a process-level wedge, not a kernel hang).

Two caveats learned the hard way:
- A single sample can land inside any cli window or IRQ handler — don't
  over-read one IF=0 sample from a *running* machine; the parked-in-HLT case
  is the meaningful one.
- Under TCG with the desktop compositing, the guest clock runs at a fraction
  of wall speed — "no output for a minute" is routinely slow, not hung.

## Batch GDB against the live kernel (process-table forensics)

For "who is blocked on what": attach, dump the thread table, detach — no
breakpoints needed. **Use `-nx`**: the repo's `.gdbinit` sets a breakpoint and
continues, which hijacks batch scripts.

```bash
gdb -nx -batch kernel/kernel.elf -ex "target remote :1234" \
  -ex "set pagination off" \
  -ex 'p thread_table[0]@12' \
  -ex 'detach'
```

Notes from use:
- Attaching PAUSES the guest — that's a feature: a consistent snapshot.
- Simple `p` expressions work well; `while`-loops over remote structs are
  unusably slow — flatten to explicit prints or one `@N` array dump.
- `thread_table[i].state` + `.wait_queue` + `.ctx.rip` (resolve with
  addr2line) answer "BLOCKED on what?"; a kernel-side selftest that
  `process_list()`s on timeout (see `test externdiag`) gets the same answer
  without any host tooling.
