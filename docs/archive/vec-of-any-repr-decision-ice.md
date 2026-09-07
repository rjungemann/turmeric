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

## Resolution (2026-09-07)

**Fixed.** All three routes to a `(Vec any)` compile and run: the reported
repro `(vec-of (id 1))`, the ascribed `(vec-of (:: 1 any))` that tripped the
separate TUR-E0201, and an explicitly ascribed `(vec-new)` pushed into.
`tests/fixtures/vec-of-any-builds` pins them under the leak harness.

Direction 1 was right about the shape -- two deciders -- and the trace named
them once `--emit-abi-trace` was actually run, which is the step this report
listed as next. They turned out to be two DIFFERENT questions that had been
answered as one:

- **Does `(Vec any)` NAME a monomorph?** `any` c-names to `tur_tagged_t`, so it
  is as nameable as `(Vec int)`. But it failed every arm of
  `adt_app_type_arg_is_concrete`, because the concrete-layout table rejects
  TY_ANY -- deliberately, and for an unrelated reason (a 16-byte by-value FIELD
  is an ABI change). So the binder fell to the int64 carrier while `repr_of`
  said typed heap pointer. Admitted in `adt_app_type_arg_is_concrete`, which is
  the predicate that asks exactly this, leaving the layout table untouched. The
  identical shape, and the identical ICE text, as the `(Vec (Opt2 int))` row
  already recorded in that function.

- **How is an `any` ELEMENT stored?** A container slot is one machine word and a
  `tur_tagged_t` is two, so erasing the element keeps the payload and drops the
  tag. `repr_of` now answers `REPR_BOXED_AGG` at `REPR_POS_CONTAINER_ELEM` --
  the same answer a by-value aggregate element has had since increment 4, which
  brings the store, the read and the element-free with it, since
  `type_is_boxed_container_elem` IS that call. Verified: `vec-free` releases
  the element boxes, LeakSanitizer clean.

One more site was needed and was NOT predicted by this report: the call-argument
carrier crossing in `emit_expr.c` is gated on `rarg.kind == TY_ADT`, so an `any`
argument reached neither the heap-box nor the stack-spill branch and the raw
`tur_tagged_t` went into `vec_push_ex`'s `int64_t` formal. An `any` has the same
problem there a by-value ADT has -- two words meeting a one-word slot -- so it
joins that block.

Direction 2 (the `vec-of` TUR-E0201) is fixed too, and it was one word:
`vec-empty-like__`'s `witness` parameter is never READ, only its type is, so it
is `^borrow`. Without that, `vec-of`'s macro binds the first element once and
uses it twice -- as the type witness and as the first push -- which an owned
`any` cannot survive. Fixing it is what made the two broken routes converge on
one defect, and it is what the layout table's own TY_ANY note said was missing
("there is no way to build one today to test it").

Residue, measured and filed as
[vec-any-monomorph-is-half-plumbed](../reported/vec-any-monomorph-is-half-plumbed.md):
`vec-get` on a `(Vec any)` still reports `int`, so the element type does not
flow back out; and a program with a `(Vec any)` beside another `vec-of` emits
one cosmetic `-Wincompatible-pointer-types` warning from a deduped `vec-new`
spec (the two monomorphs are structurally identical, verified). Neither blocks
S6; the first IS S6's subject.

Suites: `run.sh` 2862/0, `run-turi.sh` 1954/0, `run-leak-check.sh` 88/0 with one
known-open. No codegen snapshot moved.

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
