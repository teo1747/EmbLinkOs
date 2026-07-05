---
name: Build task / phase
about: A subsystem or roadmap item to build
title: "[task] "
labels: enhancement
assignees: ''

---

## What to build

<!-- The component, and where it sits in the stack / roadmap (see
     docs/ARCHITECTURE.md §2, §5). -->

## Why / what it unblocks

<!-- What's blocked or missing without it? If there's a design fork, note the
     alternatives — the decision gets recorded in docs/ARCHITECTURE.md (or a
     docs/architecture/*.md subsystem spec, for anything substantial). -->

## Concept first

<!-- The mechanism, before code. EmbLinkOs rule: understand it before building. -->

## SDM / spec references

<!-- Required if this touches hardware (paging, privilege, interrupts, TSS,
     MSRs) or an on-disk format. Intel SDM Vol/§, or EMBKFS spec §. -->

## How it will be proven

<!-- What selftest will exercise the invariant? What's the specific fact that
     distinguishes correct from broken? (Not "it runs.") -->

## Dependencies

<!-- What has to exist first? What does this block? -->

## Docs to update on completion

- [ ] `docs/PROJECT_STATUS.md` (phase status)
- [ ] `docs/ARCHITECTURE.md` (🎯 → ✅, or a decision recorded)
- [ ] `docs/TODO.md` (close the item)
