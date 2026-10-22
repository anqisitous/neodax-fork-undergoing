# NeoDAX Architecture

This document describes the current NeoDAX architecture after the static
analysis framework was ported into the main code base.

## Overview

NeoDAX is a single binary with one loader, one set of analysis passes and
one CLI. The advanced static analysis is **not** a separate program and
**not** a second type system — it is a module inside NeoDAX, reachable
through the `-w` / `--advanced-analysis` option.

```
┌──────────────────────────────────────────────────────────────┐
│                          NeoDAX                              │
├──────────────────────────────────────────────────────────────┤
│  src/main.c            CLI: flags, pass order, plugin hooks  │
├──────────────────────────────────────────────────────────────┤
│  src/static_analysis.* ADVANCED ANALYSIS (one ring)         │
│    ├── survey      sections / symbols / functions            │
│    ├── classify    variable roles                            │
│    ├── trace       call edges + data refs + caller chains     │
│    ├── predict     execution order (BFS depth from entry)    │
│    ├── parallel    dispatch / worker / beam-search groups    │
│    └── verify      re-check the previous lap                 │
├──────────────────────────────────────────────────────────────┤
│  Core passes                                              │
│  loader · disasm · x86/arm64/riscv decode · symbols ·       │
│  analysis (xrefs, functions) · cfg · loops · callgraph ·    │
│  depflow · decomp · symexec · emulate · entropy · dsa ·     │
│  unicode · daxc · hardening · plugins · interactive          │
├──────────────────────────────────────────────────────────────┤
│  include/dax.h  dax_binary_t · dax_opts_t · all public API   │
│  include/dax_guard.h  fault isolation (DAX_RUN_PASS, …)      │
└──────────────────────────────────────────────────────────────┘
```

## The Regression Ring

The advanced analysis is modelled as a single **Regression Ring**, in the
spirit of the [Bend](https://bend-lang.com) language:

- **Regression** — the ring loops. Each lap revisits the same six phases and
  checks the previous lap's findings. A dashed circle per lap plus a
  clockwise arrow shows the loop back.
- **Progression** — the evidence grows and the frontier moves outward. The
  spiral edges between laps show this.

One lap = one pass over the six phases. Two laps are run: the first
collects, the second re-checks and extends.

```
lap 1: SURVEY → CLASSIFY → TRACE → PREDICT → PARALLEL → VERIFY
                                          ↑                  │
                                          └──── regression ──┘
lap 2: (same phases, more evidence, outward spiral from lap 1)
```

## The class model

`src/static_analysis.h` defines the classes; `src/static_analysis.c`
implements them.

| Class | Role |
|---|---|
| `nd_static_ctx_t` | root object — holds the binary, the tables, the ring |
| `nd_variable_t` | a global / local, its readers and writers |
| `nd_function_t` | a function: extents, calls, reads, writes, order |
| `nd_edge_t` | a relation between two nodes (call, def-use, control, …) |
| `nd_group_t` | a group of functions: parallel, beam search, recursive |
| `nd_ring_t` | the ring: the laps, the phases, the evidence counts |

Construction / destruction / lookup are explicit methods
(`nd_variable_add`, `nd_function_find_by_address`, `nd_group_add_function`,
`nd_ring_init`, `nd_static_ctx_free`, …) so the tables never grow behind
the caller's back.

## The hierarchy in the SVG

```
ring (lap)          →  the dashed circle, the phases on its inner ring
  group              →  a box in the right-hand panel
    function         →  one row inside the box — the lowest-level group
```

Functions that belong to no detected group are collected in a `sequential`
group, so every function of the binary appears exactly once.

## Flow tracing

The `TRACE` phase resolves, for every function, the global addresses it
touches:

- **ARM64** — `adrp Xd, page` → `add Xd, Xd, #off` → `ldr`/`str [Xd]`.
  Registers are tracked and invalidated when they are overwritten, so a
  stale value never produces a false reference.
- **x86-64** — RIP-relative operands: the effective address is the next
  instruction address plus the displacement.

A resolved address is matched against the variable table (exact match
first, then the nearest symbol below within a bounded window), and the
enclosing function becomes a reader or writer of that variable. The
**caller chain** of a variable is then the chain of its readers, walked up
the call graph to the root, bounded by `--flow-depth`.

## Execution order

The predicted order is the **BFS depth from the entry function** over the
caller → callee edges. Functions at the same depth have no ordering
constraint between them: they are the candidates for concurrent execution.
Groups detected in the `PARALLEL` phase mark which of them really are
dispatched in parallel, and the group records the condition (a variable or
a message) that gates it.

## What was removed

The previous layout kept a second, incompatible `macho_binary_t` type
system under `src/core/` and spread the analysis over `src/analysis/`,
`src/flow/` and `src/tracking/`. Those modules duplicated the Mach-O parser
and the call graph, and their types did not match the ones NeoDAX uses
everywhere else. They have been removed and their useful behaviour now
lives in `src/static_analysis.c`.
