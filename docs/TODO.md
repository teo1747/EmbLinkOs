# EmbLink OS — Known Issues & Improvements

Open items only, grouped by subsystem. Completed work lives in
PROJECT_STATUS.md (and git history). Keep this file to what's actually
left to do.

---

## Bootloader

### Stage 1
- [x] Hardcoded Stage 2 sector count (8 sectors) — should be dynamic.

### Stage 2
- [x] Loads a fixed 512 sectors for the kernel regardless of actual size.
  - LESSON (learned the hard way): when the kernel outgrew the old 90-sector
    limit, the tail wasn't loaded. Symptoms were truncated MMIO addresses and
    garbled E820 — looked like a pointer bug, was actually missing code/data
    in RAM. The Makefile now warns if the kernel ELF exceeds 512 sectors.
  - PROPER FIX (bootloader v2): read the ELF header in real mode, parse the
    PT_LOAD segments (max p_offset + p_filesz), load exactly the sectors
    needed. Do this alongside the UEFI rework.
- [ ] Hardcoded kernel LBA start (sector 9) — works only because we control
  the disk layout. A real OS finds the kernel via a filesystem.
- [ ] Disk image must be padded (truncate) — workaround for reading past the
  actual kernel data. Goes away with ELF-aware loading.

### General
- [ ] No USB/CD boot — hard disk only.
- [ ] No error recovery on failed disk reads (just halts).
- [ ] BIOS only — no UEFI support.

---

## Memory Management

### PMM
- [ ] Linear scan for a free page is O(n) — slow under heavy allocation.
  Future: free list or buddy allocator.
- [ ] Stack region hardcoded at 0x200000 — should be allocated dynamically
  and tracked properly.
- [ ] Stack and PMM bitmap could collide if the stack grows > 1MB.
  - Move the stack to its own dedicated region, or allocate it via PMM after
    init, with proper guard pages.

### VMM — kernel mapping extent (NEW — today's near-miss)
- [ ] vmm_init maps only the first 2MB of the kernel into the kernel range
  (`for phys = 0; phys < 0x200000`). The kernel currently fits (~1.5MB), but
  the moment it crosses 2MB (more drivers, filesystem code, bigger BSS) the
  high addresses become unmapped → page fault on first access.
  - FIX: map dynamically up to a rounded-up KV2P(kernel_end) instead of a
    hardcoded 0x200000.

### VMM — page-table location limit (measured)
- [x] Boots fine at 4 GB. Page tables (~28 KB) + bitmap (160 KB) fit under
  physical 2 MB, so KP2V works.
- [ ] HARD LIMIT ~32 GB RAM: the PMM bitmap alone (~1 MB at 32 GB) plus page
  tables pushes allocations past physical 2 MB, where KP2V (kernel mapping is
  0–2 MB only) faults. 128 GB → ~4 MB bitmap → guaranteed crash.
  - FIX (when needed): two-phase paging bootstrap.
    * Phase 1: pre-map the first ~16 MB into the kernel range (bootloader),
      or build a minimal direct map covering where page tables will live.
    * Phase 2: build the full direct map using P2V (direct-map access).
    * Then switch VMM table access from KP2V to P2V.

### vmm_map_mmio

- [x] `vmm_map_mmio_wc()` for write-combining. `vmm_pat_init_this_cpu()`
  programs IA32_PAT entry 4 = WC (entries 0-3 keep power-on types, so every
  existing mapping is untouched); a leaf PTE with the PAT bit (`VMM_PTE_PAT`,
  bit 7) + PCD=PWT=0 selects it. Per-core, called from vmm_init + every AP
  (ap_main). The linear framebuffer now maps WC. 🪤 bit 7 is PAT on a LEAF but
  PS (huge page) on a PDPTE/PDE — `vmm_map_in` masks it out of the intermediate-
  table flags so it only lands on the leaf. Verified: `test vmm` reads PAT
  entry 4 == 0x01, fb renders, smp=4 clean.
- [x] `vmm_unmap_mmio(virt, size)` — clears the PTEs and returns the VA range
  to a small free list (`mmio_free[]`, first-fit, splits remainder) so it can be
  reused instead of leaked. `vmm_kmap_pages` also draws from it now. Verified:
  `test vmm` unmaps then re-maps and gets the SAME VA back.
- [x] Bounds check — `MMIO_END` (= `KSTACK_REGION_BASE`) is the hard ceiling;
  `mmio_reserve_va_locked` refuses to bump past it (also catches wrap) and
  returns 0 → the map fails cleanly instead of running into the kernel-stack
  region. Shared by `vmm_map_mmio[_wc]` and `vmm_kmap_pages`.

### Heap
- [x] kmalloc/kfree locking — spinlock with cli/sti save-restore.
- [x] ~~Slab allocator — fixed-size pools (16..1024) with O(1) common-case alloc
  (DEFERRED: metadata collision issues).~~ **DONE, and enabled.** The deferral
  reason is now the design. The first attempt had TWO fatal bugs:
  1. **Metadata collision** — it tagged each object `[0xAA][pool]` and hoped free
     could tell slab from general by that byte, but 0xAA occurs in real data.
     FIX: a **range registry** (`g_slab_ranges[]`) — every region records its
     `[start,end)`, and free/realloc route a pointer by RANGE lookup, which
     cannot false-positive the way a byte can. (This is the "track slab block
     ranges separately" the note asked for.)
  2. **Self-deadlock** — its free-list was a side array grown with `kheap_alloc()`
     called from *under* heap_lock, re-locking the same spinlock. FIX: an
     **intrusive free list** — a free object stores the next pointer in its own
     first 8 bytes (O(1), allocates nothing). No per-object header at all.
  A region is a permanent general allocation (`kheap_alloc_locked`, not the
  re-locking public wrapper) subdivided into objects. `slab_class_for` rounds UP
  (kmalloc(20)->32 pool) so slabs actually fire; on slab OOM / registry-full it
  falls back to general (an optimisation, never a requirement). krealloc/kfree
  range-check; kcalloc is transparent.
  Verified: `test kheap` 11/11 — every size class end-to-end, the **0xAA collision
  regression** (a general block full of the old magic byte frees correctly),
  multi-grow with no deadlock, no object aliasing, krealloc slab->general keeps
  data; kheap_check() clean after each phase. Plus: the whole kernel's small
  allocations now route through it, so booting to the desktop + `test posix` ALL
  PASS (smp=4) is thousands of live slab ops with zero corruption.
- [x] krealloc in-place expansion — checks if next block is free before allocating
  new; coalesces and returns same pointer if expansion succeeds. Reduces copies
  and fragmentation in dynamic array growth (e.g., embkfs_free_index_reserve).
- [ ] First-fit is O(n) — for allocations > 1024 bytes (small ones are now O(1)
  via the slab above). Segregated free lists by size class for the general path
  remain an option (lower priority).
- [ ] Security hardening (later): guard pages between allocations, allocation
  randomization, quarantine/delayed-free for use-after-free detection.

### Synchronization

- [x] **Sleeping mutex + counting semaphore** (`kernel/process/ksync.{c,h}`).
  Built on the wait-queue primitives that `keyboard.c`/`pipe.c`/`block.c` had
  each open-coded. A **sleeping** lock, not a spinlock: the distinction is a
  property of the CALLER (does it sleep while holding?), and the motivating case
  — the block-layer bounce buffer, held across a `hlt`-wait for the disk — does.
  Mutex is non-recursive and **detects self-deadlock** (re-lock by the owner is
  a loud halt, not a silent hang) and non-owner unlock (a warning). `test ksync`
  = 19/19 invariants; the bounce buffer now uses it (`test posix` still ALL PASS).
  - [x] Cancellation-aware acquire — `mutex_lock_interruptible` /
    `sem_wait_interruptible` return `-EMBK_ECANCELED` when the caller's process
    is cancelled while it *would block*, so a waiter behind a long-held lock
    unwinds on ^C instead of sleeping through the whole operation. The plain
    `mutex_lock`/`sem_wait` stay **uninterruptible** on purpose (a cleanup path
    must still be able to take a lock even after cancellation, and the flag is
    sticky). Cancellation gates only blocking: an uncontended acquire still
    succeeds regardless of the flag (completed-op-wins, as `process_wait`
    returns a ready child's status before considering cancellation). Proven by
    `test ksync`: a kthread blocked on a held mutex is `process_cancel`'d, wakes,
    and returns `-ECANCELED` without stealing the lock (the driver round-trips).
  - [ ] Not IRQ-safe by design (a handler cannot sleep). Spinlocks remain the
    IRQ-context tool. No compile-time guard enforces this yet.
  - [ ] No priority inheritance — a low-prio holder can be starved while a
    high-prio waiter sleeps. Irrelevant until priorities drive scheduling.

- [ ] Spinlock has no deadlock detection / lock-ordering checks.
- [x] Reader-writer locks — rwlock_t in kernel/cpu/rwlock.c (many readers OR one
  writer, single atomic state word; irqsave/irqrestore since multiple readers
  can't share one saved-flags slot). Smoke test: `test rwlock`. Not yet wired
  into any data structure (read-mostly candidates arrive with SMP).
  - [ ] Reader-preferring — a steady reader stream can starve a writer. Add a
    writer-preferring variant (pending-writer flag blocking new readers) if
    needed.
- [ ] Per-CPU heap caches to reduce lock contention (multi-core, future).

### Memory Management
- [x] EFER.NXE enabled (in gdt_init) so PTE bit 63 (NX) is legal — required
  before any VMM_NX mapping. Was missing; surfaced as a reserved-bit #PF
  (error 0x0C) the first time W^X set NX on the data segment. Consider moving
  the enable into vmm_init/CPU-feature init where it belongs logically.

---

## Interrupts & Timers

### APIC / IRQ
- [x] MADT interrupt source overrides now stored (`acpi_info.int_overrides`,
  parse_madt) and applied: `acpi_resolve_isa_irq()` maps an ISA IRQ → real GSI +
  polarity/trigger (MPS INTI flags), and `ioapic_route_isa()` routes through it.
  Keyboard/mouse/ATA switched to it; identity/edge/high when no override, so
  override-free machines are unchanged. Verified live: QEMU's `src=0 → gsi=2`
  (PIT) is parsed, and the ISA lines log their resolved GSI/trigger.
- [x] Spurious-interrupt vector (0xFF) now has an IDT handler
  (`lapic_spurious_stub`, isr.asm; installed in idt_init). Sends NO EOI (SDM:
  a spurious vector sets no ISR bit, so an EOI would ack a real interrupt) —
  a bare `iretq`. One install covers every core (shared IDT).
- [x] Spurious IRQ 7 / IRQ 15 detection — `pic_irq_is_spurious()` reads the PIC
  ISR via OCW3; `irq_handler` swallows a phantom (no EOI for a master-7, cascade
  EOI for a slave-15). Gated on NO registered handler so a real IO-APIC device
  on the same vector (ATA secondary = IRQ15 → vector 47) is never mis-dropped.
  Dormant while the PIC is masked; correct the moment a PIC line is used again.
- [x] Per-CPU LAPIC init — already in place: `lapic_init_this_cpu()`
  (MSR enable + SVR) is called by every AP in `ap_main()` (smp.c). Verified:
  smp=4 brings all APs online, each with its own LAPIC timer.
- [x] LAPIC timer TSC-deadline mode — implemented + CPUID-gated
  (`CPUID.01H:ECX[24]`), periodic LVT as the fallback. ⚠️ The active path is
  UNVERIFIED: QEMU's TCG APIC does not implement TSC-deadline and clears the
  bit on every CPU model (even -cpu max / +tsc-deadline), and no KVM is
  available here, so only the periodic path runs under our bring-up. Written to
  the SDM (Vol.3 10.5.4.1); needs KVM/real hardware to exercise.
- [x] Deduplicated the register save/restore across isr_common / irq_common /
  irq_common_lapic in isr.asm via shared `PUSH_GPRS` / `POP_GPRS` macros (the
  frame the C-side `struct registers` mirrors now has one definition).
- [x] irq_register/irq_unregister now save the line's prior PIC mask state on
  register and restore it on unregister, instead of unconditionally
  unmasking/masking (register/unregister is transparent).

### Exceptions
- [x] Page-fault handler prints CR2 + decodes the error code (P/W/U/RSVD/I)
  per Intel SDM Vol 3A §4.7.

### Timer / Time
- [x] Migrate to TSC + HPET for precise time (HPET one-shot, TSC high-res),
  both discovered via ACPI.
- [ ] Tick handler only does counter++ — eventually: preemption check +
  scheduler invocation (once there are processes).

---

## Storage

### ATA / DMA
- [x] PRD_EOT had a stray trailing semicolon (`#define PRD_EOT 0x8000;`) in
  ata.c. Works in the current single-assignment use but will break inside a
  larger expression — remove the semicolon.
- [ ] Multi-PRD scatter-gather for buffers > 64KB or spanning pages (current
  limit: single contiguous region ≤ 64KB / ≤ 128 sectors).
- [ ] LBA48 DMA (READ DMA EXT 0x25) for > 128 sectors / disks > 128GB on the
  IDE/DMA path. (AHCI path already does LBA48.)
- [x] DMA buffer no longer has to be contiguous in callers: ATA DMA now uses
  direct multi-PRD for physically contiguous mappings and a bounce-buffer
  fallback for arbitrary virtual buffers.
  For arbitrary virtual
  buffers: walk pages + multi-PRD, or bounce-buffer.
- [x] ~~Secondary channel DMA (offset 0x08 in BMIDE) not handled~~ — done
  (EMBKFS v2 Phase 21): per-channel IRQ handling (IRQ14 AND IRQ15, each
  with its own completion flag) plus a `bmide_channel_base()` helper
  (secondary channel's Bus-Master command/status/PRDT registers are at
  `BAR4 + 0x08`, not `+0x00`). Found because any I/O to a 3rd/4th drive
  was silently hanging for ~2.7 hours (`ata_wait_irq()`'s 1e6-iteration
  timeout) before this fix — see `docs/EMBKFS_spec_v2.2.md` §2.
- [ ] No cache-coherency handling (fine on QEMU/x86; matters on some HW).

### AHCI
- [ ] Polls PxCI for completion — switch to interrupt-driven (PxIS + the
  controller's interrupt) for efficiency.
- [ ] Single PRD only — add multi-PRD scatter-gather.
- [ ] Only port 0 / first device exercised — generalize across all present
  ports if multiple SATA disks appear.

### Block layer
- [x] Partition support — MBR (DOS) table parsed; each primary partition is
  exposed as a child block device (sda1, sda2…) that delegates to its parent
  disk at the partition's start LBA, bounded by its length. Mount probe sees
  partitions alongside whole disks. See kernel/block/partition.c.
  - [x] ~~GPT not parsed yet~~ **DONE** — header at LBA 1 behind the protective
    MBR, **CRC32 validated before any field is trusted** (every number we act on
    -- entry array location, count, size -- comes out of that block, so a corrupt
    one would register partitions over arbitrary sectors: fail closed). Sparse
    tables handled (all-zero type GUID = unused slot); `last_lba` is INCLUSIVE.
    ⚠️ **GPT needs CRC32-IEEE (0xEDB88320)** — `kernel/fs/embkfs/crc32c.c` is
    CRC32**C** (Castagnoli), a DIFFERENT polynomial. Reusing it because "we
    already have a crc32" would reject every real GPT on earth. `crc32_ieee()`
    now lives in partition.c.
    Verified against `sfdisk` as an independent oracle (`make part_gpt.img`):
    3/3 partitions, start+length exact.
    - [ ] Backup header (last LBA) not tried when the primary's CRC fails —
      we refuse rather than recover. The recovery is the point of the backup.
    - [ ] Entry-array CRC (`partition_entry_array_crc32`) not checked; only the
      header's.
  - [x] ~~Extended/logical partitions (type 0x05/0x0F) detected but not walked.~~
    **DONE** — the EBR chain walks; logicals register as `sda5+` (the container
    itself gets no device: nothing could mount it).
    🪤 **THE EBR TRAP: the two entries use DIFFERENT BASES.** Entry 0 (the
    logical) is relative to THIS EBR's LBA; entry 1 (the link to the next EBR) is
    relative to the EXTENDED partition's start. One base for both "works" for the
    first logical and then walks into nonsense. The walk is also bounded (32) and
    rejects self-links — a corrupt EBR must not hang the boot.
    Verified against `sfdisk` (`make part_ext.img`): sda1/5/6/7 exact.
    🪤 Names went TWO-digit: logicals start at 5 and GPT allows 128, so the old
    single digit made `sda12` collide with `sda2` — two devices, one name.
  - [ ] Scan must run after `sti` (ATA read path is IRQ-driven). Placed in
    main.c right before block enumeration / mount probe for that reason.
- [x] ~~Block-layer reads/writes on IDE secondary channel hang~~ — done
  (EMBKFS v2 Phase 21), see the ATA/DMA entry above. Disks on the
  secondary channel (3rd/4th drive) now work; FAT32's test disk staying
  on IDE primary slave is now just convention, not a hard requirement.
- [x] ~~Block-layer DMA bounce buffer is shared global state — needs a lock~~
  **DONE.** The old "fine while synchronous" note had expired: SMP, preemption
  and several processes doing file I/O (CPython/git read while a UI app runs)
  mean two concurrent bounce-path transfers would memcpy over each other and each
  return the other's sectors — **silent corruption, not a crash**. The facts
  that decided the design, established before writing it:
  - callers are `fat32.c` / `embkfs.c` / `partition.c` — **not IRQ handlers**;
  - **nothing serialises it higher up** (`embkfs` has no lock at all);
  - **the ATA path `hlt`-spins and is preemptible**, so the buffer is held across
    a multi-millisecond disk wait.
  ⚠️ **That last fact makes a spinlock WRONG** — a preempted holder would deadlock
  the next thread into the block layer on the same core ("slept holding a
  spinlock"). The kernel has no mutex, so this is a **sleeping lock built from
  the wait-queue primitives** `keyboard.c`/`pipe.c` already use: a flag + a queue,
  `{test → set}` made atomic by `g_sched_lock`.
  Notes for the next reader: the lock spans the **whole chunk loop** (releasing
  between chunks would let another caller reuse the buffer mid-transfer — the
  exact corruption being prevented), **both `rc != OK` error paths release**, and
  **pre-scheduler safety is by construction**: boot callers are single-threaded,
  so `busy` is never true, the `while` never runs, and `sched_block` is never
  reached without a thread to block.
  Verified: boots (the pre-scheduler partition scan is the thing that would hang),
  `test posix` **ALL PASS**, `test ioperf` unchanged (440 ms/MB cold, 194%
  amplification, disk still 1% of wall).
  - [ ] ⚠️ **The race itself is NOT covered by a test.** Every test above is
    single-threaded, so the lock is never contended — they prove no deadlock and
    no regression, not that the corruption is gone. A genuine concurrent-I/O
    test (two processes reading different files at once, checking neither gets
    the other's bytes) is the missing piece.
  - [ ] Bounce always copies; multi-PRD scatter-gather (page-walk via
    `vmm_get_phys`) would be the zero-copy alternative, and would retire the
    shared buffer — and this lock with it. Also: bounce always copies; multi-PRD scatter-gather (page-walk
  via vmm_get_phys) would be the zero-copy alternative (already on TODO).

---

## PCI

- [ ] BARs parsed but not stored in the pci_device struct — only printed.
  Cache them so drivers retrieve without re-reading.
- [ ] No ECAM/MCFG (PCIe memory-mapped config) — only legacy CAM. Parse the
  MCFG ACPI table, map config space, support 4KB extended config.
- [ ] No recursive bridge scanning — brute force works but doesn't follow
  secondary buses behind PCI-to-PCI bridges properly.
- [ ] No capability-list parsing (MSI/MSI-X, power management).
- [ ] No interrupt routing — wire device INTx/MSI to IO-APIC/LAPIC.
- [ ] No device-specific driver-binding mechanism yet.
- [ ] Vendor/device ID → human-name database (currently only class names).

---

## Drivers — Display / Console

### Framebuffer / VBE
- [x] ~~Real-mode VBE path hardcoded to mode 0x118~~ — done: `stage2.asm`'s
  `vbe_init` now does EDID query (INT 10h AX=4F15h BL=01h, Detailed Timing
  Descriptor #1 for native resolution) → exact-match search through the
  enumerated VBE mode list (`VideoModePtr` at VBE Info Block offset 0x0E,
  `find_mode_for_resolution` requiring ModeAttributes
  {supported, graphics, LFB} and MemoryModel 6/direct-color, preferring
  higher bpp among matches) → a fixed fallback list (1920×1080 → 1280×1024
  → 1024×768 → 800×600 → 640×480) → the original hardcoded mode 0x118 if
  literally nothing else worked. `selected_mode` defaults to 0x118 and is
  only ever overwritten by a mode `find_mode_for_resolution` actually
  confirmed exists, so any bug in the new EDID/enumeration path degrades to
  exactly the old behavior rather than something worse — deliberate, since
  stage2 has no serial debugging to fall back on if real-mode BIOS calls
  misbehave. Verified in QEMU with `-vga cirrus` (a device neither
  `bochs_vbe.c` nor `virtio_gpu.c` claims, so this path actually runs):
  picked 1280×1024×16bpp from the fallback list (Cirrus doesn't support
  32/24bpp direct-color there), rendering verified correct via screendump.
  Remaining: no scoring by "closest to native" when an EXACT resolution
  match isn't found (only the fixed fallback list is tried in that case);
  no final text-mode-at-0xB8000 fallback if VBE itself is entirely absent
  (still `.vbe_failed` → hang, unchanged from before).
  - Refs: VBE 3.0 (Function 15h DDC), EDID 1.4 (VESA E-EDID).
- [ ] UEFI GOP path (modern alternative to VBE; needs the UEFI bootloader).
- [x] ~~GPU acceleration~~ — done: `gpu.c` probes PCI for a GPU driver before
  `fb_init`; `bochs_vbe.c` does runtime DISPI modeset, `virtio_gpu.c` drives
  an accelerated guest-memory scan-out (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH
  per dirty rect). `framebuffer.c` gained a RAM backbuffer with dirty-rect
  `fb_present()`, clipped fill/copy rects, lines, circles, alpha-blit.
  Remaining: `virtio_gpu.c` only negotiates VIRTIO_F_VERSION_1 (no indirect
  descriptors, no multiple scanouts, no 3D/Virgl); `bochs_vbe.c` has no
  `-vga qxl` support. (The LFB mapping is now write-combining via
  `vmm_map_mmio_wc` — see the `vmm_map_mmio` entry above.)

### Keyboard

*(`kernel/drivers/input/keyboard.c`. Several items below were closed by work
that needed them: **Ctrl** by Ctrl-C ([INTERRUPTION.md](INTERRUPTION.md)),
**arrows/Home/End** by the EmUI text editor and the shell's history recall.)*

- [x] ~~Only ASCII press events. Add release tracking for proper modifier
  state.~~ **DONE for the keys that need it** — Shift (`0xAA`/`0xB6`) and Ctrl
  (`0x9D`) breaks are tracked. Non-modifier releases are still dropped
  deliberately: nothing consumes key-up, and a make/break API is a bigger change
  (see the open item below).
- [x] ~~No modifier handling (Shift, ..., Ctrl, ...)~~ — **Shift and Ctrl are
  handled**: `shift_down`/`ctrl_down` plus a full `scan_to_ascii_shift[]` table,
  so uppercase and `|` are typeable. **Ctrl works from BOTH keys** — right Ctrl
  arrives as `0xE0,0x1D` and is tracked identically, so `^C` works from either.
  🪤 The driver never tracked Ctrl at all until Ctrl-C needed it: **Ctrl+C simply
  typed `c`**, which made `^C` untypeable and looked like a routing bug.
- [x] ~~No extended scan codes (0xE0)~~ — **arrows, Home/End, PgUp/PgDn and Del
  are delivered** as private single-byte `EK_*` codes (`keyboard.h`), NOT ANSI
  escape sequences, so consumers need no escape state machine. Mirrored as
  `EMBK_KEY_*` in `user/lib/embk.h` — **change one, change both.**
  Also handles set-1's **fake shift**: an `0xE0`-prefixed `0x2A`/`0xAA` pads nav
  keys and must never touch real shift state.
- [x] ~~**Caps Lock**~~ **DONE** — a real latch: it flips on the MAKE and ignores
  its own break, applies to **letters only**, and **XORs with Shift**
  (Caps+Shift+a = 'a'). Applying it to the shift table — the obvious
  implementation — would make Caps type `!` for `1`, which no keyboard does.
  LED driven via `0xED`. Verified live: press → `mods=0x10`, press → `0x00`.
- [x] ~~**Alt**~~ **DONE** — `EKM_ALT`, both sides, on the event stream
  (verified live: `EKC_LALT` down → `mods=0x04`, up → `0x00`).
- [x] ~~**F1–F12**~~ **DONE** — `EKC_F1..EKC_F12` on the **event** stream. They
  get no character *by design*: C0 is Ctrl+letter's, and inventing an escape
  sequence would force every reader to become a state machine.
- [x] ~~**Ins**, **Windows/Menu**~~ **DONE** — `EKC_INS`/`EKC_LWIN`/`EKC_RWIN`/
  `EKC_MENU`, event-only for the same reason.
- [x] ~~**No key-up delivery / no make-break API.**~~ **DONE** — `struct
  key_event {code, mods, pressed}` + `sys_key_event_poll` (65) / `sys_key_mods`
  (66). A **second stream** beside the characters, not a re-encoding: the char
  stream is out of room and ambiguous (Up and Ctrl+S are both `0x13`), so text
  readers keep doing byte compares and are untouched. Still open: **typematic
  repeat-rate** control (`0xF3`).
- [x] ~~No layout abstraction~~ **DONE** — `struct keymap` + `keyboard_set_layout()`;
  **us** and **dvorak** ship. An unknown name is REFUSED, not silently ignored.
  ⚠️ **AZERTY is still not shippable, and not for want of a table**: French needs
  é è ç à ù, which are **not ASCII**, and the char stream is `char`. An "AZERTY"
  today could only be QWERTY-with-letters-moved plus silent holes — a worse lie
  than saying no. **Real AZERTY needs the char stream to carry a wider encoding
  first**; that is the actual open item, not the keymap.
- [x] ~~No PS/2 controller (8042) init~~ **DONE** — explicit config-byte RMW
  (IRQ1 on, kbd clock on, translation on) + an explicit `set 2` select.
  🪤 **Scancode set and translation are ONE decision**: device-in-set-1 with
  translation on makes the controller translate set-1 as set-2 → dead keyboard.
  We pick the standard pairing (device set 2 + translation) because our tables
  are set 1, and say so.
  🪤 **`mouse.c` RMWs the SAME config byte.** We touch only keyboard bits (0/4/6)
  and deliberately issue **no controller self-test (0xAA) and no port disables**
  (0xAD/0xA7) — every textbook init does, and here they would reset state the
  mouse depends on. Verified: `IntelliMouse wheel enabled` still appears.
- [x] ~~Migrate to USB HID once there's a USB stack~~ — done, see USB section.

---

## USB Host Controllers

All four generations now bring up their root ports and enumerate devices:
`xhci.c` (own IRQ-driven path, HID + mass storage), `ehci.c` (async
QH/qTD schedule, periodic interrupt QHs, releases FS/LS ports to a
companion), `uhci.c` (I/O BAR4, frame-list schedule), `ohci.c` (MMIO,
ED/TD lists + HCCA periodic table). `usb_core.c` is the shared HCD-agnostic
layer: enumeration, a HID boot-keyboard driver (with shift map), a
mass-storage BOT/SCSI driver that registers block devices, and hub support
(below). Legacy HCs are polled from the main loop (`usb_poll`); xHCI stays
interrupt-driven.

- [x] ~~No hub support on the legacy HCs~~ — done for UHCI/OHCI/EHCI:
  `usb_core.c`'s `usb_hub_attach` fetches the class-specific hub descriptor
  (port count), then for each port does power-on + reset + status read
  (mirroring each HCD's own root-port scan, just through class-specific hub
  requests instead of native port registers) and enumerates whatever's
  connected — single level only, with a depth guard against a malformed/
  looping topology. Verified in QEMU: a keyboard behind a `usb-hub` on UHCI
  enumerates and works (`addr 2`, boot keyboard ready). Deliberately NOT
  covered: xHCI (separate legacy code path, not `usb_core.c` — still just
  logs "USB Hub detected" and stops, matching its pre-existing "only the
  first device enumerates" limitation) and full/low-speed devices behind a
  high-speed EHCI hub (needs a Transaction Translator, EHCI spec §4.14 — a
  genuinely separate, larger feature; only same-speed downstream devices
  are enumerated). Both are documented gaps, not silent ones.
- [ ] No isochronous transfers (audio/video class devices unsupported).
- [ ] No USB3/SuperSpeed on xHCI beyond what already worked before this
  change — streams, bursting, and the SuperSpeedPlus descriptors are unused.
- [ ] Static per-controller-type tables (`UHCI_MAX_HC`/`OHCI_MAX_HC`/
  `EHCI_MAX_HC` = 2, `USB_MAX_DEVICES` = 16) — fine for QEMU testing, will
  need to become dynamic for real hardware with many devices.
- [ ] Root ports (and now hub ports) are scanned once at boot; no hot-plug
  (connect-status-change interrupt) handling anywhere in the USB stack.
- [ ] EHCI/UHCI/OHCI control and bulk transfers are synchronous busy-polls
  (bounded by a spin-count timeout, not wall-clock) — fine for boot-time
  enumeration, would stall the kernel if called after multitasking exists.
  (Multitasking now exists — see Process & Scheduling below — but nothing
  currently calls into USB from more than one process at a time, so this
  hasn't bitten yet; revisit before that changes.)
- [x] ~~No automated selftest for the display or USB stack~~ — partially
  done: `test usb` (`usb_run_selftests`, `usb.c`) cross-checks the USB
  controller table against a fresh PCI class 0x0C/subclass 0x03 scan and
  asserts every discovered controller was classified into a real HC kind
  and (if it has a usable BAR) actually initialized — deliberately
  independent of which HC/device happens to be attached, a real assertion
  about the discovery/classification code path itself. `test gpu`
  (`fb_run_selftests`, `framebuffer.c`) draws known primitives
  (`fb_fill_rect`, `fb_copy_rect`) and asserts exact colors read back via
  `fb_get_pixel`, exercising the actual color pack/unpack path for whatever
  mode is active. Neither covers live HID/mass-storage data transfer or
  actual on-screen/on-host rendering — those are still manual-QEMU-only
  (screendumps + serial-log inspection per HC generation, done this
  session for UHCI/OHCI/EHCI/xHCI and Bochs DISPI/VirtIO-GPU).

---

## Filesystem

### EMBKFS
- [x] ~~Crash while a file is unlinked-but-open LEAKS its inode~~ — the
  mount-time sweep FIX below is done: `embkfs_mount_orphan_sweep()` runs
  on every read-write mount, scanning for `links==0` inodes and reclaiming
  them. STILL OPEN (the "STRONGER" tier): an on-disk orphan list for
  crash-safe deferred delete, replayed on mount — bigger, only worth it if
  crash-safety of the unlinked-open window specifically matters.
- [ ] Open-ref table (`g_open_refs`, EMBKFS_MAX_OPEN_OBJECTS = 64) is a fixed
  array with linear scan and NO lock. SMP now exists (see Process &
  Scheduling) but nothing currently opens EMBKFS objects from more than
  one core concurrently — revisit before that changes. Same class as the
  block-layer bounce buffer below.
- [ ] `embkfs_parent_dir_oid` resolves `..` by a full-tree scan (O(tree) per
  `..`). VFS now does dot-dot itself with a breadcrumb stack, so this path is
  only hit by EMBKFS-internal callers — but if those stay, a stored parent
  back-ref would make it O(1). Low priority.
- [~] The extent-supersede bug (shrinking write → `-EMBK_EINVAL` when the
  prior data landed as ONE multi-block extent): **no longer reproducible, and
  not knowingly fixed.** Re-checked 2026-07-23 — the documented sequence
  (`test embkfs timestamps` then `test embkfs obj` on a fresh volume) passes
  *with the triggering shape present* (the log shows the 4103-byte write
  landing as `1 extent, 2 blk`, then the truncate succeeding). No fix was
  identified: `puts_cap` and the allocate/supersede loop are unchanged since
  the report (checked against 00fa091), so either something else in the
  intervening work moved it, or the trigger needed a finer bitmap state than
  "one extent vs several". **Cannot-reproduce is not fixed**, so this stays
  on the list rather than being ticked.
  - What changed instead: the invariant is now *tested*. **`test embkfs
    shrink`** runs 12 shrinking writes over whatever extent shapes the volume
    produces (truncate-to-empty, block-boundary and off-by-one cuts, a
    hole-bearing file) and checks the surviving BYTES, not just the return
    code. Verified green twice — once on a fresh volume *with* the trigger
    shape, once on a churned volume without it.
  - The lesson, recorded because it cost this investigation: the original
    repro depended on the free-block bitmap state two *unrelated* tests
    happened to leave behind. That is a coincidence with a procedure attached,
    not a repro — and it decayed into a passing sequence that could no longer
    distinguish "fixed" from "hidden". The replacement asserts the invariant
    and *reports* which shapes it saw, so a run that missed the trigger cannot
    read as one that hit it. (The first draft of the new test repeated the old
    mistake — it demanded a shape and skipped when the allocator refused, and
    7 of 9 cases went unexercised. It reported `try again` rather than green,
    which is how the flaw was caught.)

#### EMBKFS v2 (see `docs/EMBKFS_spec_v2.2.md` for full detail on all of these)
- [ ] Snapshot allocator hold-back is conservative ("hold every freed block
  while ANY snapshot exists"), not true per-block reference counting. Safe
  (never frees something a snapshot needs) but can delay reclaiming space
  that isn't actually still needed. True refcounting would need tracking,
  per block, which snapshot(s) if any still reference it.
- [x] ~~Rolling back to a snapshot reverts the snapshot registry too — any
  snapshot taken AFTER the rollback target becomes inaccessible~~ — **DONE
  (v2.3, `EMBKFS_INCOMPAT_SNAPREG`)**: the registry moved out of the CoW tree
  into one fixed block (block 1, in the pre-superblock region the formatter
  already left unused) that no transaction rewrites. Rollback swaps the root
  and never touches it, so snapshots on both sides of the target survive and
  rollback is navigable in both directions. Own CRC32C (a failed check reports
  EMPTY rather than serving bogus roots at the allocator); `bitmap_build`
  reserves the block and mkfs counts it, so the mount-time free-count oracle
  still agrees exactly. Legacy volumes without the bit keep the old in-tree
  path and the old limitation. Proof: **`test embkfs snapreg`** — s2 survives
  a rollback to s1, then rolls *forward* to s2 and back again (which also
  proves s2's frozen tree stayed physically intact), and survives a full
  bitmap rebuild. `verify_embkfs.py` §1a validates the block independently.
- [ ] Verified-root boot check uses one HMAC key embedded in the kernel
  binary (authentication against OFFLINE tampering), not real asymmetric
  signing (which would also defend against someone who has the kernel
  image itself). A true upgrade (Ed25519 or similar) is its own
  crypto-primitive-sized body of work on top of the existing SHA-256/AES.
- [ ] AES-256-XTS has no ciphertext stealing — every write happens to
  always be block-size-rounded today, so it's never been needed, but a
  future call site that ISN'T block-rounded would need it added.
- [ ] Crypto header (Phase 4) and verify header (Phase 5d) both live past
  the superblock's own CRC32C checksum coverage (`EMBKFS_SB_BODY_SIZE`) —
  deliberate (corruption there fails closed, not open) but means a
  corrupted crypto/verify header isn't caught by the SAME mechanism that
  catches a corrupted superblock body; each has its own magic-number
  sanity check instead.

### VFS
- [x] ~~Single mount only~~ — done: `vfs_find_mount` does real longest-prefix
  matching (`vfs_mount_is_prefix` + a `best`/`best_len` scan) across up to
  `VFS_MAX_MOUNTS = 8` slots. Landed alongside EMBKFS v2's multi-volume
  mounting (each volume gets its own `/<device_name>` mount point).
- [ ] No `.readlink` op. Symlinks resolve to the LINK vnode but can't be
  followed; EMBKFS already has `embkfs_readlink_object`.
  - FIX: add `.readlink` to vfs_ops + adapter, follow links inside
    `vfs_resolve` with a hop-count bound (ELOOP). NOTE: true `..` then differs
    from the lexical breadcrumb `..` (a symlink's real parent vs its path
    parent) — revisit the stack semantics when this lands.
- [x] ~~No `.truncate` op → O_TRUNC can't be honored at open().~~ **DONE** —
  `vfs.h` has `.truncate(vn, size)`, `vfs_fd_truncate()` exposes it as
  `ftruncate(2)`, and O_TRUNC is wired in `vfs_open`. A filesystem that leaves
  the op NULL fails an O_TRUNC open with `-ENOSYS` rather than silently opening
  a file it did not truncate. Driven by git, which ftruncates its index.
- [ ] stat() simplifications: directory nlink hardcoded to 2 (real = 2 +
  subdir count); directory size reported as ENTRY COUNT, not bytes (ls tags it
  'e'). Decide whether anything depends on either before "fixing."
- [x] ~~`unlink` op has remove(3) semantics — it rmdirs an empty directory.~~
  **DONE** — `embkfs_unlink()` returns **`-EISDIR`** on a directory; `rmdir` is
  a separate, dirs-only path that returns `-ENOTEMPTY` on a populated one.
  (The v2.2 `unlink` was worse than mis-specified: it was a **stale lie** that
  reported success without removing anything. git found it.)
- [x] ~~Absolute paths only — no cwd / relative resolution.~~ **DONE**, but
  deliberately NOT in the kernel: the VFS stays the one absolute-only path
  parser and never sees a relative path. cwd is **libc state**
  (`user/lib/syscalls.c`), which makes it per-process for free — every process
  already has its own libc data, so there is no kernel state to leak. Not
  inherited: a parent names `PWD` and the child's crt0 seeds from it.
  `path_abs()` also NORMALIZES (`/a/../b` used to reach the kernel raw and die
  on a `..` the VFS has no rule for — git walks UP to find a repo root).
  Still open: `fchdir()` is honest `ENOSYS` — an fd does not know its own path
  (`fd_entry` holds mnt+ino, and nothing maps an ino back to a name).
- [ ] `vfs_mount` duplicate-mount check is a raw strcmp on `at` (not a
  normalized path). Fine for the single "/" mount.

### File descriptors
- [x] ~~fd table is fixed (64), global, per-boot. No per-process fd tables~~ —
  done: `struct fd_entry` moved to `kernel/fs/fd.h` (public) and `struct
  process` now embeds `struct fd_entry fds[FD_MAX_OPEN]`. `fd.c` picks the
  table via a `fd_table()` helper — `current_process->fds` if there is a
  current process, else a boot-time-only `g_boot_fds` (preserves the
  existing pre-process `test ring3`/selftest behavior that runs before any
  process exists). Verified via QEMU: two spawned processes opening
  different files see independent fd numbering/state, no cross-talk.
  Open-file-description sharing (fork / dup / dup2, shared cursor across
  dup'd fds) is still NOT modeled — each fd entry still owns its own
  cursor; only isolation between processes was added, not fd aliasing
  within one.
- [x] ~~g_boot_fds / per-process fds arrays are unlocked mutable state — needs
  a lock once syscalls from the same process can race.~~ **DONE**, and the race
  was already REACHABLE, not hypothetical: the note's "a single process only
  runs one syscall at a time" was **stale**. `sys_thread_create` (14) exists, and
  the scheduler picks any READY thread per core (only `pinned_cpu` threads are
  core-locked) — so two threads of one process run on two cores and race the
  shared `fds[]`. The open path was the worst: it found a free slot, did a
  possibly-blocking `obj_get`, and only THEN marked the slot used — a wide window
  for two threads to claim the same slot.
  Fix: a per-process **`struct mutex fd_lock`** (the new ksync mutex — a SLEEPING
  lock, because obj_get/close do I/O). `fdlock(p)` keys on the process (the
  install paths mutate a TARGET's table) and no-ops for the boot context (NULL =
  g_boot_fds = single-threaded). Open now claims the slot atomically; close frees
  under the lock then tears down OUTSIDE it on a snapshot (mandatory: pipe close
  takes g_sched_lock, which the mutex is built on). The one rule — never take
  fd_lock under g_sched_lock — holds at all five sites.
  Verified: `test posix` ALL PASS on **smp=4** (heavy open/close/read/write, no
  deadlock), `test thread smp` OK (8 threads/one proc — the process_alloc change).
  - [ ] ⚠️ **The race trigger is still not covered by a test** — every suite is
    effectively single-threaded per fd table, so the lock is never contended.
    Same gap the bounce-buffer lock has; a concurrent open/close hammer (N
    threads, assert no two fds alias one slot) is the shared missing harness.
- [x] ~~O_TRUNC reserved but not honored.~~ **DONE** — `fd.c` shrinks to zero
  through the VFS truncate op at open time.
- [ ] O_APPEND is implemented by re-stat-to-end before each write — one extra
  stat per write, and a TOCTOU window between the stat and the write. **NOT
  fixed by the fd_lock above** — that guards the fd TABLE, but two O_APPEND
  writers (even in different processes, or two fds to one file) race on the
  file's CONTENTS, not the table. A correct fix is FS-level: an atomic
  append-at-end op (or a per-object append lock) in the write path, not fd.c.
  A per-fd lock would give false confidence (misses the two-fds-one-file case).
  Now genuinely reachable (threads on SMP), so no longer just theoretical.

---

## User Mode & Syscalls

### ELF Loading (COMPLETED)
- [x] ELF64 loader (kernel/cpu/elf.c/h) loads PT_LOAD segments with correct
  permissions (PF_X → no NX, PF_W → writable). Maps pages in user VA space
  (0x0000400000000000–0x0000700000000000 low-half), restores real p_flags
  after loading (NX on data, exec on code).
- [x] Entry point (e_entry) from ELF header used instead of hardcoded
  0x400000. init_blob is now an ELF-format binary, not flat .bin.
- [x] Context save/restore (kernel_ctx_save / kcontext) allows kernel to
  resume after user program exits via sys_exit.

### Ring-3 Entry & Exit (COMPLETED)
- [x] iretq path to ring 3 with user code/data selectors, user RSP, EFLAGS=0x202
  (IF+reserved). Works with interrupt handling + IRQ re-enable on exit.
- [x] sys_exit (syscall 2) restores saved kernel context → control returns to
  enter_user_mode (no halt).
- [x] Interrupts live during user execution (no CLI mask); IRQs pre-enabled
  before entering ring 3.

### Security — user-pointer validation (HIGH, the real hole) — DONE
- [x] ~~`sys_write` dereferences a raw user pointer with NO validation~~ — done:
  `kernel/cpu/usercopy.c/h` adds `access_ok(ptr, len)` (checks the range is
  below the canonical low-half boundary `0x0000800000000000` and every page
  in it is actually mapped in `current_process->pml4_phys` via
  `vmm_get_phys_in`), plus `copy_from_user`/`copy_to_user` (access_ok +
  memcpy, `-EMBK_EFAULT` on failure) and `copy_string_from_user` (bounded,
  byte-at-a-time, for path arguments). Wired into every syscall that takes a
  user buffer or path (`sys_write`, `sys_read`, `sys_open`, `sys_stat`,
  `sys_readdir`, `sys_spawn`). Verified with a temporary test in
  `user/init.c`: a normal buffer round-trips correctly, and a deliberately
  bad pointer (a kernel address) is rejected with `-EMBK_EFAULT` instead of
  being dereferenced — confirmed via QEMU, then the test scaffold was
  reverted.

### Userspace loader
- [x] elf_load partial-load cleanup now happens by loading into a fresh
  address space and destroying that address space on load failure.
  but segment N+1's frame alloc fails, segment N's pages stay mapped and
  nothing unmaps them. Harmless in the single shared address space today;
  becomes a real leak once per-process address spaces exist (a failed load
  must discard the whole attempt). FIX: unwind mapped segments on error, or
  build into a fresh address space that's simply dropped on failure.
- [x] Loads /init.elf from the filesystem (multi-block extent read verified).
  Embedded-blob path removed.

### Syscall transport
- [x] `int 0x80` (software gate) implemented: dispatch table now has 13 calls
  — write=1, exit=2, yield=3, open=4, close=5, read=6, lseek=7, stat=8,
  readdir=9, spawn=10, wait=11, getpid=12, kill=13. `spawn` returns a
  capability handle (not the raw pid); `wait`/`kill` take one.
- [ ] Fast path `syscall`/`sysret` deferred: needs STAR/LSTAR/SFMASK MSRs + EFER.SCE,
  AND swapgs + a per-CPU GS base for the kernel-stack switch (syscall does NOT
  switch stacks via the TSS). Wants per-CPU/SMP infra. GDT already laid out user
  data-before-code for it.
- [x] ~~Syscall table: wire the fd/VFS syscalls~~ — done: `sys_open`,
  `sys_close`, `sys_read`, `sys_lseek`, `sys_stat`, `sys_readdir` (via a
  callback-based `sys_readdir_cb` walking `vfs_readdir`, filling a
  `struct sys_dirent { ino, type, name[59] }` per entry) all go straight
  through to the existing `vfs_*`/`vfs_fd_*` layer, guarded by
  `copy_from_user`/`copy_to_user`/`copy_string_from_user` (see
  user-pointer validation above).
- [x] ~~`sys_write` is serial-only and ignores fd~~ — done: fd==1 still goes
  straight to serial (no fd-table round trip needed for the common case);
  any other fd routes through `vfs_fd_write` via a bounce buffer.
- [x] Found bug (not on this list originally): `sys_exit` read the exit code
  from `r->rax` (the syscall number — always 2) instead of `r->rdi` (the
  actual argument), so every process's exit code was hardcoded to 2
  regardless of what it passed. Fixed to read `r->rdi`; verified exit code
  changes correctly (6 for `/init.elf`'s counter+bss-sum check).
- [x] Found bug (not on this list originally): `int 0x80` is an interrupt
  gate, so IF is auto-cleared for the entire syscall handler — any syscall
  that waits on a hardware completion IRQ (e.g. disk I/O inside `sys_open`)
  hung forever. Fixed by adding `sti` as the first instruction of
  `syscall_dispatch()`.

### Ring-3 / TSS plumbing
- [x] Ring-3 entry point: enter_user_mode() loads ELF, maps user stack,
  saves kernel context, enters ring 3 via iretq.
- [x] ~~`tss_set_rsp0` exists but RSP0 is a single static kernel stack~~ — done:
  `schedule()`/`process_start_first()` call `tss_set_rsp0(next->kstack_top)`
  before every switch. See "Process & Scheduling" below for the subsystem
  this now belongs to.

### Per-process address spaces
- [x] vmm_destroy_address_space walks the user half, frees mapped frames and
  user page-table pages, and is called on ELF load failure, stack setup
  failure, and normal process exit.
- [x] Per-process PML4 (kernel half aliased 256/511, user half private);
  vmm_map_in/get_phys_in; late CR3 switch; higher-half p_vaddr rejected.

---

## User Interface (EmUI), Compositor & Userland Runtime

Design/usage docs: `docs/EMUI_GUIDE.md` (how to build an app),
`docs/EMUI_INTERNALS.md` (how the toolkit is built), `docs/BUILD_SETUP.md`
(newlib + dynamic linking). Open items only — the toolkit itself, the
compositor, and dynamic linking are all built and live-verified; what
follows are known gaps and rough edges, not missing features. Since
2026-07-23 the OS also **builds** EmUI apps on itself (`test tcc dyn`):
tcc compiles and dynamically links against `libembk.so`, and the widget
renders — and since the same day EmbBuild **builds** one from a manifest
(`test embbuild gui`, `/data/src/ui/build.ebm`), so the rebuild-self claim
covers the GUI, not just static C. See `docs/PORTS.md` § "The GUI wall, and
how it came down" and BUILD.md §6.

- [ ] **Only ONE EmUI app has a build manifest** (clockw). home/uidemo/wmdemo
  and the rest of `user/bin/*.c` are still host-built only. This is now
  breadth, not capability — each needs the same three-stanza shape plus its
  own header closure.
- [ ] **Header inputs in manifests are hand-written and can go silently
  stale.** The clockw manifest's first draft listed 8 of its 12 transitive
  headers; it would have built fine and skipped rebuilds on a `backend.h`
  edit. Auto-depfiles (BUILD.md §2.4's deferred item) are the fix; until
  then, derive the list from the include graph rather than from the top of
  the .c file.

- [ ] **Dynamic linker: eager relocation only, no lazy PLT binding.** Every
  `R_X86_64_JUMP_SLOT` is resolved at load time in
  `kernel/arch/x86_64/syscall/elf.c`, not on first call. Simpler and
  currently fine at this app count/size; revisit if process start latency
  becomes a real cost.
- [ ] **A residual, unexplained transient `EFAULT` under SMP** in
  `copy_from_user`/`copy_to_user` (`kernel/arch/x86_64/syscall/usercopy.c`)
  — `access_ok` occasionally reports a genuinely-mapped user page as absent
  under `-smp 4`, cause not found (suspects: a `this_cpu()`/APIC-ID
  misattribution window, or a memory-ordering gap in the page-table walk
  that only TCG's multi-threaded emulation exposes). Currently masked, not
  fixed: both copy functions retry `access_ok` up to `USERCOPY_RETRIES` (8)
  times before actually reporting a fault, which has been reliable in
  practice but is a workaround, not a diagnosis. If unrelated EFAULTs
  reappear elsewhere, start here.
- [ ] **EMBKFS has latent (not yet triggered) SMP hazards from the UI
  bring-up's heavier concurrent I/O.** Several `embkfs.c` functions use
  `static uint8_t probe[4096]`/`datablk[4096]` scratch buffers shared across
  calls, and the per-volume whole-object read cache (`vol->rcache_*`) is
  unlocked — both are use-after-free/data-race risks if two cores genuinely
  race a read against that cache's replace/free path. Worth a per-volume
  lock at the `embk_vfs` bridge before apps start doing more concurrent
  filesystem I/O than they do today.
- [x] ~~**`mkfs_embkfs.py`'s single-leaf image builder overflows past ~7
  packed files.**~~ — done: `build_btree()` (with `pack_items_into_leaves()`)
  greedily packs metadata items into as many level-0 leaves as they need and
  adds a level-1 internal root above them when there's more than one leaf; the
  root stays at block 17 so the superblock pointer is stable. `make_image`
  auto-splits (a two-pass build learns the metadata-block count, then places
  the data region after it — leaf packing is invariant to data-block
  placement since an extent's `disk_block` is fixed-width). The
  `wgyehkb.txt`/`illoeuw.txt` collision-chain fixtures are restored to both
  images. Verified: the oracle passes, and the kernel MOUNTS + DESCENDS the
  level-1 tree live (`root node OK level 1 (internal) nritems 2`), lists all
  13 files, resolves the collision chain, and launches an app read entirely
  through the 2-level tree — no exceptions. `make_tree_image` keeps its
  deliberate forced 2-leaf split as a fixed descent regression. *(Remaining
  headroom: one internal node holds ~72 leaf slots; a level-2 tree isn't
  implemented and `build_btree` raises if an image ever exceeds that — far
  off at current app counts.)*
- [x] ~~**Overlay/modal flagged unstable in live interaction.**~~ —
  root-caused and fixed. The "modal doesn't render / half-drawn scrim" was
  NOT a hang or a tree/layout bug (it renders correctly on the host, and the
  live dirty-rect union was already the full window). The scrim is a
  **full-window semi-transparent solid** (`a=0.55`), and `cpu_draw_rect`'s
  fast interior path was **opaque-only** (`a >= 0.999`) — so every one of the
  ~425k scrim pixels took the per-pixel float `blend_over`. Under TCG that's
  the documented "float-per-pixel = poison": one modal frame took ~25s, so
  screenshots caught it mid-render (scrim covering only the top rows, dialog
  not reached yet). Fix: a **constant-alpha integer-LUT fast path** in
  `cpu_backend.c` (`out = round(dst*(1-a)) + round(src*a)`, carry-safe, four
  256-entry LUTs) — benefits EVERY translucent solid, not just the scrim.
  Result: first modal frame ~3s render (~8x faster), renders cleanly.
  Verified with an always-open-modal `EM_APPLICATION` app (retained runtime)
  live — dialog + scrim correct, no corruption, no exceptions; host render
  pixel-identical, all 6 UI suites pass. Also fixed alongside: the retained
  runtime (`em_app.c`) now includes `em_overlay_active()` in its build gate,
  so a modal in an `EM_APPLICATION`/`EM_WIDGET` app keeps building frames
  while it's up (needed for the scrim-dismiss debounce `g_ov_frames >= 3` to
  advance, and for any modal animation).
- [x] ~~**Adding a new app requires three manual registration points.**~~ —
  done: the build system now auto-discovers apps. A Makefile pattern rule
  (`build/%.elf` over `$(EMUI_APPS) := $(wildcard user/bin/*.c)` minus the
  special-linked `init.c`/`hello.c`) builds any `user/bin/*.c` as a
  dynamically-linked EmUI app, and `mkfs_embkfs.py`'s
  `discover_userland_objects()` globs `build/*.elf` and packs them all.
  Adding an app is now just "drop `user/bin/foo.c` in, `make embkfs.img`" —
  verified by dropping a throwaway `probeapp.c` and watching it compile,
  link, and land on the image with zero build-file edits. *(Minor residual:
  deleting an app's `.c` leaves a stale `build/*.elf` that keeps getting
  packed until `make clean` — adding is free, removing needs the clean.)*
- [ ] **No real TTY.** The framebuffer console is output-only (no
  scrollback, no line editing); a text shell (see Architecture roadmap)
  needs this built first.
- [ ] **`home.elf` plays an informal init/service-manager role** (spawns and
  tracks every app the user launches, including the clock widget at boot)
  without being a real PID-1/service-manager abstraction — fine for a
  single-user desktop today, a real gap if multiple always-running services
  are ever needed.

---

## Process & Scheduling

Full phased spec, comparative analysis (Linux/Windows/BSD/XNU), and every bug
below in detail: `docs/architecture/process-and-scheduling.md`. Current
mechanism: `thread_table[MAX_THREADS=256]` (schedulable unit) split from
`process_table[MAX_PROCESSES=64]` (resource owner), real SMP (per-CPU
`current_thread`, AP bring-up, one global `g_sched_lock` held across the
context switch), timer-driven preemptive priority-band round-robin (100Hz
LAPIC tick, 4 bands + aging), wait queues for blocking, an uncatchable
`process_kill`, real parent-blocked `process_wait()`, per-process fd
tables, per-process ring-3 handle tables, and joinable ring-3 threads
(`thread_create_user`/`thread_join`) sharing one process's address space.
`process_create()` builds a fresh address space + page-mapped
guarded kernel stack from an ELF path and fabricates a first context that
lands in `process_trampoline`, which `iretq`s to ring 3. The kernel's own
interactive shell (`main.c`) is itself a real process now
(`process_adopt_current()`), with `run`/`ps`/`kill`/`wait`/`nice` commands.
Phases B, C, and D (see roadmap in the architecture doc) are all now
substantially complete.

- [x] ~~Ring-3 entry `#GP` on every process start~~ — `process_trampoline`'s
  inline-asm `iretq` frame pushed the literal `$2` instead of the `%2`
  operand (the real `0x23` selector) for CS. Loading CS from the resulting
  null-descriptor selector faulted immediately. Fixed.
- [x] ~~`kstack_top` truncated to 16 bits~~ — `struct process::kstack_top` was
  `uint16_t` but stored a 64-bit heap address; `tss_set_rsp0()` read the
  truncated field. Would have corrupted RSP0 on the first ring-3 interrupt.
  Now `uint64_t`. Fixed.
- [x] ~~Zombie processes never reclaimed~~ — nothing transitioned
  `PROCESS_ZOMBIE` back to `PROCESS_UNUSED`; after exactly `MAX_PROCESSES`
  exits, `process_create` would fail forever. Fixed: `process_reap()`, deferred
  one `schedule()` call behind the actual exit (can't free a stack still being
  executed on). Not yet exercised in practice — `main.c` only ever creates one
  process today, so the deferred-reap path has never actually fired; needs
  either a second process or the `test sched reap` selftest below to prove it.
- [x] ~~No kernel-stack guard page~~ — `alloc_kernel_stack` was a flat
  `kmalloc`, silently corrupting whatever sat next to it on overflow. Fixed:
  `vmm_alloc_kernel_stack`/`vmm_free_kernel_stack` page-map the stack with an
  unmapped guard page directly below it.
- [x] ~~Kernel-stack region invisible in the first process's own page
  tables~~ — introduced *while fixing* the guard-page bug above: the new
  region was placed in a PML4 slot untouched before boot.
  `vmm_create_address_space()` shares the kernel half by copying PML4 entries
  *by value* at creation time, not by live reference — a slot that's
  not-present at that moment stays not-present in that process forever, even
  after the kernel's own table fills it in moments later. `#PF` on first
  touch, which can't even push its own fault frame (same region backs
  `TSS.RSP0`) → `#DF`. Fixed by reusing `MMIO_BASE`'s already-populated PML4
  slot (256 GiB in, clear of the real MMIO bump allocator) instead of a fresh
  one.
- [x] ~~No preemption~~ — done: `lapic_timer_handler` (`lapic.c`) now calls
  `schedule()` after `lapic_send_eoi()`, at the existing 100Hz tick rate.
  Genuine timer-driven round-robin, verified via `test sched roundrobin`
  (two kthreads interleave without either calling `sys_yield`/`sys_exit`).
- [x] ~~No blocking / wait queues~~ — done: `struct wait_queue { struct
  process *head; }` plus an intrusive `wait_next` link on `struct process`.
  `wait_queue_block()` marks the caller `PROCESS_BLOCKED`, links it in, and
  calls `schedule()`; `wait_queue_wake_one`/`wait_queue_wake_all` unlink and
  mark `PROCESS_READY`. Verified via `test sched roundrobin`'s blocking
  variant.
- [x] ~~No priority~~ — done: 4 fixed bands (`PRIORITY_REALTIME`/
  `INTERACTIVE`/`NORMAL`/`BACKGROUND`, 0=highest), round-robin within a
  band, aging (`PRIORITY_AGE_TICKS = 20`, ~200ms/band) bumps a starved
  READY process up one band so a busy high band can't starve a low one
  forever. Verified via `test sched priority`. Note the aging period was
  deliberately kept short (200ms, not a "nicer-looking" multi-second
  value) — the worst case (a BACKGROUND process recovering all the way to
  REALTIME contention) needs `SCHED_PRIORITY_BANDS - 1` full periods, and
  at a multi-second period that's long enough for even the kernel's own
  shell (PRIORITY_NORMAL) to visibly freeze if a spawned child ever ran
  busier and higher-priority than it.
- [x] ~~No uncatchable kernel-level kill~~ — done: `process_kill(pid)`
  (`process.c`) forces `PROCESS_ZOMBIE` regardless of current state, unlinks
  the target from any wait queue it's blocked on, and reaps it immediately
  unless it's the currently-running process (in which case it defers via
  the existing `g_pending_reap` mechanism, same as a normal exit). Matches
  `docs/ARCHITECTURE.md` §3.3's requirement that a process which never
  cooperates can still be stopped. Verified via `test sched kill`.
- [x] ~~No per-process file descriptor table~~ — done, see "File
  descriptors" under Filesystem above (`struct process::fds`, `fd_table()`
  helper in `fs/fd.c`).
- [x] ~~No `sys_wait`/`sys_spawn` syscalls~~ — done, and since upgraded past
  the original busy-poll: `sys_spawn` calls `process_create()` on a
  user-supplied path (via `copy_string_from_user`) and returns a
  **capability handle**, not the raw pid (see the handle-model bullet
  below); `sys_wait` resolves the handle and calls `process_wait()`, which
  genuinely **blocks** the caller on the target's parent's `child_wait`
  queue until that specific child exits or is killed — no more polling.
  `sys_yield` wired to syscall number 3; also added `sys_getpid` and
  `sys_kill`. Verified via a temporary `user/init.c` scaffold exercising
  the full handle lifecycle (spawn → handle, wait on a bad handle →
  `-EINVAL`, wait on the real handle → correct exit code, wait on the
  now-freed handle again → `-EINVAL`); reverted after verification.
- [x] ~~Real blocking `sys_wait` (was Phase D in the architecture spec)~~ —
  done: `process::parent`/`parent_pid`/`zombie_head`/`zombie_next`/
  `child_wait` (`process.h`) implement the hand-off. `parent_pid` exists
  specifically because `parent` is a raw pointer into a slot that gets
  recycled after reaping — checking `state != PROCESS_UNUSED` alone can't
  tell "still my real parent" from "an unrelated new process reused the
  slot," a real bug caught while building this (`parent_is_alive()`).
  **Found and fixed a real deadlock getting this exercised for the first
  time**: `schedule()`'s zombie hand-off ran *after* the "nothing else
  runnable" early return, but the one scenario `process_wait()` creates is
  exactly a parent sitting `BLOCKED` (not READY/RUNNING) with nothing else
  runnable at that moment — so the hand-off that would wake it never ran.
  `test sched wait` hung on its very first run; fixed by moving the
  zombie-handling decision before the "is there anything else to run"
  search. Full postmortem: `docs/architecture/process-and-scheduling.md`
  §16, Bug 11.
- [x] ~~Ring-3 process handles (`docs/ARCHITECTURE.md` §3.4/§3.5)~~ — done:
  each process has its own `struct proc_handle handles[PROC_HANDLE_MAX]`
  table; `sys_spawn` allocates a handle pointing at the new child's real
  pid and returns the handle, `sys_wait`/`sys_kill` resolve their handle
  argument back to a pid before acting. Closes the confused-deputy gap a
  raw pid argument left open (any ring-3 process could otherwise name any
  pid it could guess) — this was explicitly flagged as a known, deliberate
  divergence earlier in this file and is now closed.
- [x] ~~No `sys_kill`~~ — done: exposes the already-existing (Phase B)
  `process_kill()` to ring 3 via the handle table. Does not free the
  handle — the caller still needs it to `sys_wait()` afterward and collect
  the "killed" exit code (-1).
- [x] ~~Kernel has no way to actually launch/manage processes
  interactively~~ — not originally on this list, but became a real gap
  once `main.c`'s old single-hardcoded-process auto-launch
  (`process_start_first()`, a one-way hand-off) was removed: nothing
  called `process_create` at all anymore, and the removal left behind a
  stray unconditional inner `for (;;) hlt` loop that trapped the shell
  after at most one keystroke. Fixed: `main.c` now calls
  `process_adopt_current()` to make the interactive shell itself a real,
  permanently-scheduled `current_process` (not a one-way hand-off), with
  new `run <path>` / `ps` / `kill <pid>` / `wait <pid>` / `nice <pid>
  <priority>` shell commands calling straight into `process.c`'s
  kernel-internal API (no handle indirection needed there — trusted code,
  not a sandboxed ring-3 caller). Verified interactively in QEMU via
  monitor-injected keystrokes: `run /init.elf` spawns a real child as the
  shell's sibling, `ps` shows both with correct PID/PPID/state/priority,
  `wait` blocks and correctly retrieves the real exit code, `kill`/`wait`
  on an already-exited or nonexistent pid are safe no-ops.
- [x] ~~`process_create`/`proc_alloc` both increment `next_pid`~~ — fixed:
  removed the redundant `proc->pid = next_pid++` from `process_create()`;
  `proc_alloc()` is now the sole assigner.
- [x] ~~No automated selftest~~ — done: `test sched roundrobin`, `test sched
  kill`, `test sched reap`, `test sched stackguard` (`process_test_*` in
  `process.c`, wired into `selftests.c`), matching the four selftests
  `docs/architecture/process-and-scheduling.md` §12 specifies. `reap` in
  particular exercises the deferred-reap path noted above, which nothing
  had actually triggered before (main.c previously only ever created one
  process). Two more added alongside priority scheduling and real blocking
  wait: `test sched priority` (a REALTIME kthread dominates a BACKGROUND
  one, but aging still rescues the BACKGROUND one from total starvation)
  and `test sched wait` (process_wait()'s two hand-off paths: normal exit
  and uncatchable kill).
- [x] ~~Single core only~~ — done (SMP phase, see `docs/architecture/
  process-and-scheduling.md` §13). `current_thread` is a real per-CPU
  field (`cpu_table[]`); a single global `g_sched_lock` guards
  `thread_table`/`process_table`/`current_thread`/`pending_*_reap`, held
  ACROSS the context switch itself (released only by whichever thread
  resumes on the far side) — the load-bearing part, not just "add a
  lock." Per-CPU run queues remain deliberately deferred (§8/§17) until
  this single lock is measured to bottleneck.

### New bugs found while building preemption/syscalls/spawn (not on this list originally)
- [x] **IF=0 leak into every voluntary-block wake** (ledger Bug 26, the
  full write-up lives in docs/architecture/process-and-scheduling.md).
  `sched_block_current_locked()` sleepers resumed with interrupts off whenever
  a *timer-initiated* switch-in woke them; the leak reached `ata_wait_irq`'s
  bare `hlt` and froze the machine. Ten call sites had accumulated without the
  IF-restore Bug 25 declared a "documented rule" — the fix is now structural
  (`sti` inside the primitive itself), `ata_wait_irq` uses `sti;hlt` + a loud
  canary, and `test writestorm` (200 write/unlink cycles + IF probes, any leak
  fails) pins it. Found while porting tcc headers: the probabilistic freeze
  masqueraded as "tcc hangs on-OS" until the QEMU monitor's `info registers`
  showed `RIP` at ata.c's `hlt` with `RFL.IF=0`.
- [x] **RFLAGS.IF corruption in context switch.** `kernel_ctx_switch`/
  `kernel_ctx_save` (`kcontext.asm`) capture RFLAGS via `pushfq`/`pop rax`
  and are always called from inside an interrupt-gate ISR (timer IRQ,
  `int 0x80`), which auto-clears IF on entry — so the *live* flags always
  read IF=0 at the capture point, regardless of the outgoing process's true
  state. Saving that corrupted snapshot meant every resumed process came
  back with interrupts permanently masked, hanging after its first
  preemption. Found via `test sched roundrobin` (scheduler hung after one
  cycle). Fixed by forcing `or rax, 0x200` (set IF) before storing to the
  RFLAGS field in both functions — safe because reaching that save point at
  all proves the process had IF=1 moments earlier (a maskable IRQ can't
  fire with IF=0, and ring-3 can't execute cli/sti, it's privileged and
  would #GP).
- [x] **`int 0x80` leaves IF=0 for the whole syscall.** Same root cause as
  above, different symptom: any syscall that blocks on a hardware
  completion IRQ (disk I/O inside `sys_open`/`sys_read`/`sys_spawn`'s ELF
  load) hung forever because the IRQ that would satisfy it could never
  fire. Found while verifying file I/O syscalls. Fixed with `sti` as the
  first instruction of `syscall_dispatch()`.
- [x] **`proc_alloc()` race between allocation and initialization.** Found
  while verifying `sys_spawn`: with real preemption now active, a second
  `process_create()` call (from a live syscall context) could be preempted
  mid-setup during its slow ELF-load disk I/O — but `proc_alloc()` had
  already marked the new slot `PROCESS_READY` immediately, so the scheduler
  could pick a half-initialized PCB (no pml4/kstack/ctx yet) and crash.
  Fixed: `proc_alloc()` now marks the slot `PROCESS_BLOCKED` instead;
  `process_create()`'s own final step is the only place that sets
  `PROCESS_READY`, after everything is actually built.
- [x] **PCB leak on `process_create()` error paths.** Found via code review
  while fixing the race above: none of the four early-return failure paths
  (address-space creation, ELF load, user-stack alloc, kernel-stack alloc)
  reset `proc->state` back to `PROCESS_UNUSED`, so a failed `process_create`
  permanently leaked the PCB slot. Fixed by adding the reset to all four
  paths.
- [x] **`schedule()` was reentrant against the timer ISR when called from
  syscall context.** Found by re-reading the finished preemption/syscall
  work (not by a crash): `syscall_dispatch()`'s `sti` (needed for the
  `int 0x80` fix above) meant `schedule()` calls from `sys_exit`/`sys_yield`
  ran with IF=1, so a timer IRQ could land mid-`schedule()` and re-enter it
  while the outer call was still mutating shared scheduler state. Fixed:
  `schedule()` now `cli`s at entry (saving the caller's IF) and restores it
  on every early-return path, the same `pushfq`/`cli`/conditional-`sti`
  idiom `cpu/spinlock.c` already uses elsewhere.
- [x] **`schedule()`'s zombie hand-off deadlocked the first real use of
  blocking `process_wait()`.** The hand-off/auto-reap decision for a dying
  process was only reachable *after* confirming some other runnable
  process existed — true for every scenario that existed before
  `process_wait()`, but false the instant a parent can be `BLOCKED`
  (not READY/RUNNING) waiting specifically for this exit with nothing else
  runnable. `test sched wait` hung on its very first run. Fixed by moving
  the hand-off decision before the "anything else to run" search.
- [x] **`parent` pointer could alias an unrelated process after PCB slot
  reuse.** Found while implementing the parent/child tracking above:
  `struct process::parent` is a raw pointer into the static process table;
  checking only `parent->state != PROCESS_UNUSED` to mean "still alive"
  breaks the moment the real parent exits, is reaped, and a totally
  different new process gets allocated into that same recycled slot — the
  state check would wrongly read as "alive" for the wrong process, handing
  an exit off to a stranger. Fixed by also storing `parent_pid` (the
  parent's pid at the moment `parent` was set) and comparing
  `parent->pid == parent_pid` too (`parent_is_alive()`).

---

## Core / Library

- [ ] Refactor done: kprintf + snprintf share one format_string core. Future:
  add %b (binary) / field-precision if needed; both wrappers benefit.
- [ ] kstring: only the subset in use is implemented. Add more (strstr, strtok,
  memchr, etc.) as needed — don't pre-build the whole libc.

---

## Architecture (big-ticket, later)

- [x] ~~SMP — multi-core bring-up~~ — done: AP startup (INIT-SIPI-SIPI,
  `kernel/cpu/smp.c` + `ap_trampoline.asm`/`ap_entry.asm`), per-CPU data
  (`kernel/cpu/percpu.c/h`, `cpu_table[]`), per-CPU LAPIC/GDT/TSS. Full
  detail: `docs/architecture/process-and-scheduling.md` §13. Per-CPU RUN
  QUEUES specifically are still deferred (see Process & Scheduling above)
  — a real, separate, deliberately-scoped-out next step.
- [ ] Portability / HAL discipline — keep new upper-layer code arch-neutral
  (no inb/outb, no direct page-table pokes, no x86 asm in logic); route
  arch-specific operations through arch_* interfaces. Real ARM64 port is a
  later dedicated campaign — don't pre-abstract against a single architecture.
- [ ] **embbuild** — the native build tool (the make-equivalent — DECIDED and
  now DESIGNED: `docs/BUILD.md` is the ratification document + v1 spec;
  manifest format, content-hash stamps, stage/adopt via atomic rename,
  `/data/build/` tree, `test embbuild` acceptance).
  The fork was audited and ratified: a **native structured tool**, not a make
  port. The deciding facts, each verified against the tree: every userland
  recipe is one argv = one `spawn()` (no `/bin/sh` buys nothing); the host
  Makefile cannot run on-OS regardless (no compatibility payoff); the RTC's
  one-second mtimes against millisecond TCC compiles make timestamp staleness
  structurally false-fresh (→ staleness by **content hash** of inputs + argv).
  Shape: targets as typed records (name, sources, `-I`, objects, link inputs,
  install path), recipes as argv arrays, `/data/src/<project>/` as the source
  convention, the ABI as ambient constants — the schema `test tcc tally`
  already executes hand-unrolled. Deliberately absent from v1: variables,
  pattern rules, parallelism (the real graph is ~50 explicit nodes). Honest
  rebuild-self scope with TCC: static newlib C, **and `libembk.so` GUI apps
  since 2026-07-23** (`test tcc dyn`); still no `__thread` (no linker
  scripts/PT_TLS), no C++, kernel wants GCC.
  make itself arrives later as opt-in compat with the foreign-tree ports
  story. Nice detail available: build it on the sval SDK, which is on-image
  and already proven self-rebuildable.
  - [ ] v1 staleness detail: hash file bytes in userspace; exposing a cheap
    content identity from EMBKFS's CoW generation machinery is a later kernel
    item, pulled by need.
  - [ ] Prerequisite already DONE: separate compile-then-link with
    tcc-produced objects, proven live (`test tcc tally`).