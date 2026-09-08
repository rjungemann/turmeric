---
title: A `(Vec any)` builds and runs, but its element type does not flow back out and its `vec-new` spec dedups against a sibling
category: Reported
description: With the repr-decision ICE fixed, a (Vec any) can be constructed, pushed into and freed correctly -- but `vec-get` reports its result as `int` rather than `any`, so an element cannot be read back with its tag; and a `vec-of` producing a (Vec any) emits -Wincompatible-pointer-types, which run.sh's emitted-C ratchet fails.
---

# `(Vec any)` is buildable now, and half-plumbed

**Severity: medium.** Neither half is a miscompile -- the values stored are
correct and the element boxes are freed -- but the first makes a `(Vec any)`
write-only, which is most of the point of having one, and the second means only
one of the three routes to building one is shippable.

Residue of
[vec-of-any-repr-decision-ice](../archive/vec-of-any-repr-decision-ice.md),
which is fixed: `(Vec any)` no longer aborts the compiler. These are the two
things that fix did not reach, both found by measuring what the newly-buildable
type actually does.

## Half 1 -- `vec-get` on a `(Vec any)` returns `int` -- FIXED 2026-09-07 (S6)

**Fixed on the compiled path.** Two changes at the two ends of the same value:
`call_result_type`'s collapse of a bare-tyvar result to the int64 carrier now
exempts `any`/`union` (they are the two-word `tur_tagged_t`, so there is no
single word to reinterpret back from -- the reason that comment already gives
for the composites beside them); and the `any` readers bridge a carrier-form
operand back to the aggregate, since a boxed element arrives as the slot word.
`tests/fixtures/vec-any-element-roundtrip` pins int/cstr/float/bool each
reporting its own type.

The INTERPRETER now diverges instead, and worse -- it answers `bool` for all
four. Filed as
[vec-any-interp-keeps-one-element-tag](vec-any-interp-keeps-one-element-tag.md).

The original account follows.

### Original

```turmeric
(defn dyn [x : any] : any x)
(defn main [] : int
  (let [v (:: (vec-new) (Vec any))]
    (vec-push! v (dyn 7.1))
    (println (type-of (vec-get v 0))))
  0)
```

```
error: 'type-of' expects an 'any'-typed argument, got 'int'
```

Identical on both back ends, so this is elaboration, not codegen. The element
type does not flow out of the container: `vec-get [A] [v : (Vec A) i : int] : A`
should ground `A` to `any` from the receiver and does not.

Ascribing past it does NOT recover the element -- it makes it silently wrong,
which is the part worth knowing:

```turmeric
(println (type-of (:: (vec-get v 0) any)))   ; compiled: int   interpreted: float
```

That ascription is a WIDEN of an `int`-typed expression, so it re-tags the
carrier word with a statically-chosen tag rather than reading the element's own.
The two back ends pick different tags, which is how the shape announces itself.
The ascription is behaving correctly in isolation; what is wrong is the type it
is handed.

This is S6's subject (containers of `any` are that stage's whole point) and it
should be fixed there rather than as a local patch, since the same question --
"how does an element type reach a read through a container" -- governs
`#map{...}`, `#set{...}` and cons lists too.

## Half 2 -- a `vec-of` producing a `(Vec any)` does not clear the emitted-C ratchet

**Corrected twice. This is not cosmetic, and `tests/run.sh` says so.**

A `vec-of` that produces a `(Vec any)` emits:

```
warning: returning 'tur_adt_Vec__int *' from a function with incompatible
         return type 'tur_adt_Vec__any *' [-Wincompatible-pointer-types]
```

`run.sh` greps every fixture's build stderr for exactly
`-Wint-conversion` / `-Wincompatible-pointer-types` and FAILs on a hit. That
gate was sweep-verified at ZERO across 2563 fixtures before it landed
(`docs/archive/emitted-c-pointer-integer-warnings-unwatched.md`), so a hit is a
regression signal, not noise -- and "the two monomorphs have identical layout,
so the pointer is the same pointer" was the wrong thing to conclude from. It is
a hard error under `-Werror`.

I got this wrong in two stages, both by not asking the suite:

1. First recorded as "cosmetic" on a layout comparison alone.
2. Then narrowed to "only when a `(Vec any)` sits beside another `vec-of`",
   from manual `tur run` probes at `-O1`. `run.sh` builds at `-O2` and fails a
   SINGLE `vec-of` at `any`.

So only ONE route to a `(Vec any)` is currently shippable -- an explicit
`(:: (vec-new) (Vec any))`, which `tests/fixtures/vec-of-any-ascribed` pins.
The `vec-of` macro route builds and runs correctly but cannot be a fixture.

**This now BLOCKS S6's headline feature.** `[1 "two" 7.1]` in a Saffron file
lowers to `(vec-of ...)`, so G7 (a vector literal defaulting to `(Vec any)`)
cannot land while a single `vec-of` at `any` trips the emitted-C ratchet. It was
filed as a residue; it is on the critical path.

### Cause, as far as it is established

Inside `vec-empty-like__`'s `any` clone, the zero-argument `(vec-new)` call
resolves to `vec_new__spec__tur_adt_Vec__int__` even though
`vec_new__spec__tur_adt_Vec__any__` is emitted and is called correctly from
elsewhere in the same program. A probe on `emit_abi_intern_spec` confirms both
specs are CREATED (`SPEC-NEW vec-new result=(type-app Vec int)` and
`... (type-app Vec any)`), so the intern is not the problem -- the call site is.

`emit_call_name` (emit_core.c ~3482) short-circuits a zero-argument call
BEFORE the spec-matching loop:

```c
if (call->kind == EX_CALL &&
    (call->as.call_.n_args == 0 || (b && b->is_construct_template))) {
    ...
    return raw_name_for_binding(b);
}
```

**Established 2026-09-08, and it is not the short-circuit.** The call does not
reach it: it takes the exact-match path above and matches the WRONG entry.

That table is keyed on `(Expr*, active outer spec)`, which is right --
`vec-empty-like__`'s body is ONE Expr tree emitted once per clone, so a
per-`Expr*` key alone could hold only one answer. The defect is the CROSS-SPEC
FALLBACK beside it:

```c
if (ctx->specialized_call_outer[i] == active_outer) { matched = ...; break; }
/* Cross-spec fallback only inside a spec (active_outer != NULL) ... */
if (active_outer != NULL && !saw && !construct_into_carrier) {
    matched = ctx->specialized_call_names[i];
    saw = true;
}
```

Inside `vec_empty_like____spec__..._any` the `(vec-new)` Expr has an entry
recorded under the `int` clone's outer and NONE under the `any` clone's, so the
fallback takes the sibling -- a clone whose return ABI is `tur_adt_Vec__int *`
where this body returns `tur_adt_Vec__any *`. The comment directly above already
names that hazard for the NULL-outer case ("so it never routes a call to a
spec-scoped clone with a different return ABI") and does not guard the non-NULL
one.

### Narrowed further, 2026-09-08 -- four layers, each measured

Chasing "why is there no recording under the `any` outer" ruled out three
candidates and landed inside one function. Each step is a one-line `fprintf`,
run as `TUR_PROBE_REC=1 ./build/tur emit-c <file>` on
`(defn main [] : int (println (vec-len (vec-of (:: 2 any)))) 0)`:

1. **Not the recorder.** `emit_abi_record_specialized_call` keys on
   `(Expr*, active outer)` correctly, and IS called for the `any` clone of
   `vec-empty-like__` itself:
   `REC vec-empty-like clone=..._any_... outer=(NULL)`.
2. **Not the recursion guard.** The scan of a freshly-minted spec body
   (`if (fd && fd->body && ctx->n_abi_specializations != before_specs)`) RUNS
   for the `any` clone -- all three conditions true:
   `RECURSE? fd=1 body=1 newspecs=1 clone=..._any_...`.
3. **Not the intern.** The `int` clone's recursion reaches the intern site
   (`REACHED-INTERN vec-new result=(type-app Vec int) outer=..._int_...`); the
   `any` clone's produces NO such line.
4. **So the decline is upstream of the intern, inside `emit_abi_scan_expr`'s
   `EX_CALL` case**, and it is specific to `(Vec any)` -- `(Vec int)` walks the
   identical path in the same program and proceeds.

That is where the next probe goes: bisect the guards in that case for a
zero-argument callee whose result is a concrete app over `any`. The lookup-side
fallback below is then a SYMPTOM, and fixing it would only mask a missing spec.

The alternative fix -- **guard the cross-spec fallback on return ABI**, the way
`construct_into_carrier` guards it for a different reason -- stays available and
stays second choice: it treats the symptom, and declining the fallback drops the
call to the carrier base, which then has its own int64-vs-pointer straddle to
survive.

## Fix directions

1. **Half 1, in S6**: ground a container read's element tyvar from the
   receiver's monomorph. Worth doing once for every container rather than for
   `Vec` alone.
2. **Half 2**: find why the per-Expr* recording that resolves a zero-argument
   call names the `int` clone inside the `any` clone's body. The intern side is
   ruled out (both specs exist); a probe on that recording is the step, and
   reading the call sites did not settle it -- twice.
