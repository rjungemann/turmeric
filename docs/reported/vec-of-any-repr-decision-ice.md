---
title: A `(Vec any)` built from a call returning `any` ICEs the compiler -- "a representation decision disagrees with repr_of at binding"
category: Reported
description: `(vec-of (id 1))` where `id : any -> any` aborts with an internal compiler error -- want=heap-ptr got=carrier-i64 for `(type-app Vec any)`. Two sites decide the Vec's representation differently. Pre-existing (reproduces in plain Turmeric with explicit annotations); the interpreter runs the same program correctly.
---

# `(Vec any)` from a call ICEs the compiler

**Severity: high.** An internal compiler error is never an acceptable failure
mode -- it is an abort with a "please report this"-shaped message, not a
diagnostic a user can act on. The program is also not obviously wrong: putting
dynamic values in a vector is an ordinary thing to want.

Found while landing
[saffron-lang-plan](../upcoming/saffron-lang-plan.md) S2, whose whole change is
to default an unannotated parameter to `any`. **It is not caused by S2** -- the
same program written in plain Turmeric with explicit annotations ICEs
identically -- but S2 makes the shape trivially reachable, since in a Saffron
file every unannotated function already produces `any`.

## Repro (2026-09-07) -- plain Turmeric, no dialect involved

```turmeric
(defn id [x : any] : any x)
(defn main [] : int
  (println (vec-len (vec-of (id 1))))
  0)
```

```
$ tur run p.tur
tur: internal error (ICE): a representation decision disagrees with repr_of at binding.
  repr-shadow binding let-bind type=(type-app Vec any) want=heap-ptr got=carrier-i64
  cty=int64_t own=int64_t

$ tur --interpret p.tur
1
```

The interpreter runs it, so the semantics are not in question -- only the
compiled representation decision is.

## What narrows it

| program | result |
| --- | --- |
| `(vec-of 1 2)` -- concrete element | works (`2`) |
| `(vec-of (id 1))` -- element from a call returning `any` | **ICE** |
| `(vec-of (:: 1 any))` -- element ascribed to `any` in place | a *different* wrong answer: `TUR-E0201 cannot copy unique value '__vw'`, raised inside `stdlib/vec.tur:492` |

So it takes an `any` that arrives **from a call** to reach the ICE; an `any`
produced by an in-place ascription trips a separate defect in `vec-of`'s own
macro expansion instead. Both paths are broken, differently, and neither
reports something a user could act on.

## Root cause (partial -- the disagreement is reported, not diagnosed)

The ICE is the repr-shadow check firing: `(type-app Vec any)` is decided as
`heap-ptr` at one site and `carrier-i64` at another. That check exists precisely
to catch this family, and its message points at
`docs/archive/repr-decision-function-plan.md` as the plan for closing it.
`TUR_REPR_NO_SHADOW_ICE=1` downgrades it to a warning, which is the
bisection handle, not a fix.

Which two sites disagree is not established here -- `--emit-abi-trace` prints
every disagreement and is the next step.

## Fix directions

1. **Find the two deciders and make one call the other.** That is the
   repr-decision-function plan's whole thesis; this is one more instance of the
   family it names, and worth landing there rather than as a local patch.
2. **Fix `vec-of`'s uniqueness error on an ascribed `any` separately** -- the
   third row above is a different defect that happens to share a symptom
   (a `(Vec any)` you cannot build). It is in `stdlib/vec.tur`'s macro, not in
   the representation layer.

Blocks S6 (containers and the Saffron prelude), where a `(Vec any)` is the
ordinary case rather than a corner. Does not block S2-S5, which move dynamic
values around without containerising them.
