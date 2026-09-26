---
title: Diagnostics on parametric stdlib containers print compiler internals, not source spellings
category: Reported
description: A payload mismatch on vec-push! / chan-send reads "expected tyvar, got float", and a handle mismatch reads "expected (type-app Chan tyvar 'A')". Neither is a spelling a user can write. Pre-existing on Vec; S2 widened it to chan, ref and atomic.
---

# Diagnostics on parametric stdlib containers print compiler internals

**Severity: low** (diagnostic quality; no wrong answers), but it lands on the
**most common mistake** the new parametric containers invite -- putting two
payload types in one container.

**RESOLVED 2026-09-26** -- directions 1 and 2; direction 3 (say where `A`
was pinned) is not done and is not tracked separately.

- **Direction 1.** The argument-mismatch diagnostic in `elab_call_fn_inner`
  (`src/compiler/elab_call.c`) now takes the declared parameter type when the
  expected slot is a bare type variable, and substitutes the call's type
  bindings into it before printing. The repro reads
  `function 'vec-push!' arg 2: expected int, got float`; a variable that is
  still unbound prints under its own name.
- **Direction 2.** `type_name_buf` (`src/compiler/types.c`) prints a type
  application in its source spelling, flattened along the spine:
  `(Chan A)`, `(Map K V)`, `(Option int)` -- no `type-app`. A named type
  variable prints as its name (`A`) instead of `tyvar 'A'`; the name alone
  still tells cross-skolem mismatches apart. `type_name`'s `TY_APP` arm routes
  through the same printer. The hole-headed partial-application arm
  (`(F _ X)`) is unchanged.

Pinned by `errors/vec-push-bound-payload-mismatch` (the repro below) and by
the `errors/{atomic,chan,ref}-wrong-handle`,
`errors/heap-parametric-self-typed-field-int-terminator`,
`errors/instance-applied-result-mismatch` and `errors/set-of-heterogeneous`
snapshots, which now spell `(AtomicCell A)`, `(Chan A)`, `(Node int)`,
`(Option Pt)` and `expected int, got cstr`. `tests/run-repr-trace.sh` matches
the new spelling. No `expected.c` snapshot moved.

**Status at filing:** OPEN. Filed 2026-09-18 while executing
[stdlib-int-stand-in-audit](../reported/stdlib-int-stand-in-audit.md) S2. **Not a
regression from that work**: it reproduces on `vec-push!`, which has been
parametric all along. S2 widened its reach to `chan`, `ref` and `atomic`.

## Repro

```turmeric
(defn main [] : int
  (let [v (vec-new)]
    (vec-push! v 42)
    (vec-push! v 7.25)      ;; pinned to int by the first push
    (vec-free v))
  0)
```

```
error [TUR-E0001]: function 'vec-push!' arg 2: expected tyvar, got float
```

`expected tyvar` is the internal kind name. What the user needs to be told is
`expected int` -- the type `A` was unified with one line earlier -- and ideally
where it got pinned.

The handle form is the same defect at the type level:

```
error [TUR-E0001]: function 'chan-send' arg 1:
  expected (type-app Chan tyvar 'A'), got (type-app AsyncChan tyvar 'A')
```

`(type-app F X)` is `type_name_buf`'s general fallback
(`src/compiler/types.c:3295`). The surface spelling is `(Chan A)`. Note the
sibling branch immediately above it already prints the partial-application form
as `(F _ X)`, so the nicer shape exists for one case and not the general one.

## Why it matters more now

Before S2 these APIs took `:int`, so a wrong payload produced
`expected int, got float` -- correct and actionable. The parametric signatures
are a strict improvement in what they ACCEPT (float and by-value aggregates now
work at all) and a regression in what they SAY when they reject. Three
`errors/*-wrong-handle` fixtures had their expected text updated to the
`(type-app ...)` spelling in that change; they are the pin for whatever this is
fixed to.

## Fix directions

1. **Resolve the tyvar before printing.** At the point of the mismatch `A` is
   already bound; report the binding (`expected int`) rather than the variable.
   This is the half that matters -- it is the common case.
2. **Print `(F X)` instead of `(type-app F X)`** in `type_name_buf`. Cheap, but
   it touches every diagnostic and fixture snapshot that currently spells
   `type-app`, so it wants its own change with the regen in it (and
   `type-app` may be deliberate in the ICE text at `types.c:848`).
3. If (1) lands, say where `A` was pinned. The first `vec-push!` is the
   interesting line, and the error points at the second.
