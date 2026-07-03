---
name: Build task / phase
about: A subsystem or roadmap item to build
title: "[task] "
labels: enhancement
assignees: ''

---

## What to build

<!-- The component, and where it sits in the stack / roadmap (see
     ARCHITECTURE.md §2, §5). -->

## Why / what it unblocks

<!-- What's blocked or missing without it? If there's a design fork, note the
     alternatives — the decision gets recorded in ARCHITECTURE.md. -->

## Concept first

<!-- The mechanism, before code. Helios rule: understand it before building. -->

## SDM / spec references

<!-- Required if this touches hardware (paging, privilege, interrupts, TSS,
     MSRs) or an on-disk format. Intel SDM Vol/§, or EMBKFS spec §. -->

## How it will be proven

<!-- What selftest will exercise the invariant? What's the specific fact that
     distinguishes correct from broken? (Not "it runs.") -->

## Dependencies

<!-- What has to exist first? What does this block? -->

## Docs to update on completion

- [ ] `PROJECT_STATUS.md` (phase status)
- [ ] `ARCHITECTURE.md` (🎯 → ✅, or a decision recorded)
- [ ] `TODO.md` (close the item)
