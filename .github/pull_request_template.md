## What this changes

<!-- One or two sentences. Which layer/phase, and what it does. -->

## Why

<!-- The justification. What was blocked or missing before this? If it's a
     design decision with alternatives, say why this approach over the others. -->

## How it was proven

<!-- EmbLinkOs rule: a change isn't done because it compiles or prints "hello" —
     it's done when a selftest exercises the actual invariant. Describe the
     test and what it proves. Quote the key output if useful. -->

- [ ] Builds clean (`make`)
- [ ] Boots and runs in QEMU (state which target: `make run` / `run-embkfs-cow` / …)
- [ ] Selftest added or updated that exercises the invariant this change is about
- [ ] The proof is a real assertion, not just "it didn't crash"

Proof:
<!-- e.g. "read-after-unlink returns the original bytes through an open fd;
     blocks reclaimed only on last close" — the specific thing that would FAIL
     if the change were broken. -->

## SDM / spec references

<!-- If this touches hardware (paging, privilege, interrupts, TSS, MSRs) or an
     on-disk format, cite the section reasoned from. Intel SDM Vol/§, or the
     EMBKFS spec §. Leave blank if pure software with no hardware contract. -->

## Docs synced

- [ ] `docs/PROJECT_STATUS.md` updated if a phase completed or state changed
- [ ] `docs/ARCHITECTURE.md` (and the relevant `docs/architecture/*.md` spec,
      if one exists for this subsystem) updated if a design decision landed
      or a 🎯 became ✅
- [ ] `docs/TODO.md` updated — new gaps added, closed items removed

## Known gaps left behind

<!-- EmbLinkOs discipline: name what this DOESN'T handle rather than pretend it's
     complete. What's the next thing that will have to deal with the loose end?
     (These become docs/TODO.md entries.) -->

## Commit hygiene

- [ ] Commits are milestone-sized with messages explaining the *why*
- [ ] Nothing stray uncommitted (`git status` clean)