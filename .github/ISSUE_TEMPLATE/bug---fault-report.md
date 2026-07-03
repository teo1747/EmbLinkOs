---
name: Bug / fault report
about: Something faults, corrupts, or misbehaves at runtime Create a report to help
  us improve
title: "[bug] "
labels: bug
assignees: ''

---

## What happens

<!-- One or two sentences. What did you expect, what happened instead. -->

## Fault dump (if it faults)

<!-- Paste the full exception output. In EmbLinkOs you diagnose by READING the
     dump, so this is the most important field. Include: -->
Vector:      <e.g. 0x0E Page Fault>

Error code:  <the bits — P / W / U / RSVD / I-D>

RIP:

CS / SS:     <low 2 bits = which ring>

CR2:         <fault address, for #PF>

CR3:         <which address space, if relevant>

## What the dump tells you

<!-- Decode it before proposing a fix. e.g. "P=0,W=1,U=0,CR2=0x400000 →
     kernel wrote an address not mapped in the active CR3." -->

## Repro

- Target: <`make run` / `run-embkfs-cow` / …>
- Steps:

## Suspected layer

<!-- Which subsystem — VMM, fd layer, EMBKFS, syscall, loader, etc. -->
