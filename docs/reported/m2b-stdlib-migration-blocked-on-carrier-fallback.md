---
title: M2b stdlib `ok`/`err`/`some`/`none` migration blocked on the carrier-fallback emit path
category: Codegen / ABI — M2b follow-up
severity: Medium. Blocks Phase 2 of `docs/upcoming/m2b-make-struct-design.md` (rewriting stdlib constructor bodies to `make-struct` / `default-of`). The Phase 1 surface infrastructure ships; Phase 2 must wait on M4 (orthogonal monomorphization for the typeclass-instance-method dispatch context) per the design's own punt.
description: After Phase 1 of M2b landed (`make-struct` keyword form, `default-of` core form, EX_DEFAULT_OF emit, plus a narrowly scoped recovery patch that lets the make-struct compound literal pick up the enclosing ABI spec's result-type when the body's own `(default-of B)` leaves a struct tyvar unbound), I attempted to rewrite stdlib `ok` / `err` to the new body form. The monomorphized clones build correctly and tests like `(:: (ok 42) (Result int cstr))` round-trip end-to-end. But the compiler also emits a **generic carrier body** for every `#{Construct}` polymorphic defn — `static int64_t ok(int64_t x) { ... }` — and that generic emit has no `current_abi_specialization`, so my recovery patch can't fire. The body lowers to `return (int64_t){.is_ok = true, .ok_val = x, .err_val = (int64_t){0}};` which the C compiler rejects (designated initializer on a non-aggregate).
status: RESOLVED 2026-06-13 (same session). Implemented option (a) in three coordinated patches: (i) `emit_fns.c` synthesizes a carrier-shaped body (`return tur_<helper>((int64_t)(intptr_t)x);`) for any `#{Construct}` make-struct defn whose result type lowers to the int64 carrier (covers both the generic carrier-only emit and typeclass-dispatch carrier-return specs); (ii) `emit_fns.c`'s box-spill recognizer (`needs_box_spill`) now fires for `#{Construct}` make-struct bodies as well as inline-C ones, so the existing prereq-6 by-value synthesis covers value-struct payloads through make-struct bodies too; (iii) `emit_module.c`'s carrier-skip gate (which previously only matched inline-C bodies) now also matches `#{Construct}` make-struct bodies, so ABI-neutral calls keep using the carrier symbol rather than monomorphizing into a by-value spec that the caller's dispatch ABI wouldn't accept. Stdlib `ok` / `err` / `some` / `none` are migrated to the new body form; `some` / `none` are polymorphized over `[A]`. Full suite (~1442 fixtures): 170 FAIL vs 172 pre-existing baseline (only diff: a transient `hamt-delete` flap; no migration-induced regressions).
---

# M2b stdlib migration is blocked on the carrier-fallback emit path

## Context

M2b Phase 1 introduces the `(make-struct StructName :field-key v ...)` keyword
surface, the `(default-of T)` core form, and an `EX_DEFAULT_OF` node lowered as
`(T){0}`. See `docs/upcoming/m2b-make-struct-design.md`.

Phase 1 also includes a targeted emit-side recovery patch in
`src/compiler/emit_expr.c` (the `EX_MAKE_STRUCT` case): when the make-struct's
result type carries unresolved tyvars (because, e.g., `(default-of B)` is the
only place `B` appears in the body and the ABI spec's `bindings[]` doesn't
bind `B`), the emit walks the enclosing
`current_abi_specialization->result_type` and uses its concrete struct name +
spine args to (a) name the compound literal's outer cast, and (b) re-type any
`(default-of T)` field value that would otherwise lower to `(int64_t){0}` as
the concrete spine type (`(const char *){0}` etc.).

This recovery works for every **monomorphized clone**. The Phase 2 attempt
was to rewrite stdlib `ok` / `err` to use the new body so they could be
exercised on the new pipeline:

```turmeric
(defn ok [A B] [x : A]
  #{Construct}
  : (Result A B)
  (make-struct Result :is-ok true :ok-val x :err-val (default-of B)))
```

Smoke tests with concrete ascriptions pass:

```turmeric
(defn main [] : int
  (let [r (:: (ok 42) (Result int cstr))
        e (:: (err "oops") (Result int cstr))]
    (println (.ok-val r))  ; => 42
    (println (.err-val e)) ; => oops
    0))
```

But the full suite regressed by ~5 fixtures, all flavors of "build failed":

```
FAIL hkt-stdlib-result-ok-biased            — tur build failed
FAIL polymorphic-ok-err-value-struct-payload — tur build failed
FAIL result-of-typed-eq                      — tur build failed
FAIL result-typed-basic                      — tur build failed
FAIL typeclass-return-dispatch-result-wrapped — tur build failed
```

All five trace to the same generated C:

```c
static int64_t ok(int64_t x) {
    return (int64_t){.is_ok = true, .ok_val = x, .err_val = (int64_t){0}};
}

static int64_t err(int64_t e) {
    return (int64_t){.is_ok = false, .ok_val = (int64_t){0}, .err_val = e};
}
```

```
error: initialization of non-aggregate type 'int64_t' (aka 'long long')
       with a designated initializer list
```

## Root cause

The compiler emits two flavors of body for a polymorphic `#{Construct}` defn:

1. **Monomorphized clones**, one per spec — e.g.
   `static Result__int__cstr ok__spec__Result__int__cstr_int64_t(int64_t x)`.
   These get a non-NULL `current_abi_specialization` on the EmitCtx; the
   recovery patch fires and the cast lands as `(Result__int__cstr){...}`.

2. **A generic carrier body**, one global — `static int64_t ok(int64_t x)`.
   This is emitted with `current_abi_specialization == NULL`. It exists as a
   fallback for callers that need the unspecialized symbol (typeclass-method
   dict dispatch, separate-compilation handles, generic-unsafe relays in
   neighbouring polymorphic bodies). At emit time the make-struct's `e->type`
   is `(Result A B)` with bare tyvars; `type_c_name(TY_APP)` falls back to
   `int64_t`; the body becomes invalid C.

The pre-M2b inline-C body for `ok` is exactly the right shape for the
carrier-fallback path: `return tur_ok((int64_t)(intptr_t)x);` — int64 in,
int64 out, tagged via the runtime helper. The Phase 2 rewrite has no
equivalent escape hatch.

The design itself acknowledges this in
[m2b-make-struct-design.md](../upcoming/m2b-make-struct-design.md), under
"What this design doesn't address":

> **HKT-class method bodies** -- `fmap` for Option/Result returns a freshly-
> constructed sum via `tur_some` / `tur_ok`. After M2b those bodies can switch
> to `make-struct` too, but the *dispatch* still goes through the typeclass
> dict; that's M4 territory, not M2b's.

and "Migration plan from M2a":

> The carrier-return path (typeclass-instance-method dispatch context) is
> handled by the orthogonal monomorphization work in M4, not by retaining
> inline-C bodies.

In other words, the design assumes the carrier-fallback emit will be
**eliminated** by M4 (every constructor call monomorphized away). Until M4
lands, the carrier body still has to be emitted, and it has to compile.

## Repro

With Phase 1 + the emit-side recovery patch in tree, apply this delta to
`stdlib/result.tur`:

```diff
 (defn ok [A B] [x : A]
   #{Construct}
   : (Result A B)
-  ```c
-  return tur_ok((int64_t)(intptr_t)x);
-  ```)
+  (make-struct Result
+    :is-ok true
+    :ok-val x
+    :err-val (default-of B)))
```

Then `bash tests/run.sh`. The five fixtures above fail with the same
`non-aggregate designated initializer` error in the generic `static int64_t
ok(int64_t x)` body.

`(:: (ok 42) (Result int cstr))`-style direct calls (single-file `tur build`)
still pass — those only need the monomorphized clone.

## What's needed to unblock Phase 2

Two orthogonal options:

**(a)** Make the carrier-fallback emit valid for `#{Construct}` defns with
`make-struct` bodies. The carrier ABI's contract is "in: int64, out: int64
tagged for the runtime helpers." The natural lowering of a `(make-struct
Result :is-ok true :ok-val x :err-val (default-of B))` body in carrier mode
is `return tur_ok((int64_t)(intptr_t)x);` — exactly what the inline-C body
already says. The emit could synthesize this from the StructDef shape (one
`:bool` discriminator field + one payload field whose value comes from the
single payload param) without keeping inline-C in stdlib. This is the same
shape M2a's prereq-6 path in `emit_fns.c:629-787` already infers, so the
code is largely already written — it just needs to fire in the carrier-
emit path too, not only the box-spill specialization.

**(b)** Eliminate the carrier-fallback emit for `#{Construct}` defns
entirely (M4). Once every call site monomorphizes through a known dict,
there are no callers of the generic carrier symbol and we can stop emitting
it. This is what the design assumes will happen.

Option (a) is a smaller, immediately-shippable patch and lets Phase 2 land
ahead of M4. Option (b) is the strategic direction the design points at.

## What landed anyway

Phase 1 of M2b ships in this session (see the new fixtures
`tests/fixtures/m2b-make-struct-keyword`, `tests/fixtures/m2b-default-of`,
plus three `errors/m2b-make-struct-*-field` diagnostics, all PASS). The
emit-side recovery patch ships too — it has no effect when the recovery
condition doesn't apply, and it fixes the monomorphized clone for any
future caller that writes a `make-struct` body with phantom return-type
tyvars (the same class of bug as
[make-struct-phantom-typeparam-lowering.md](make-struct-phantom-typeparam-lowering.md),
addressed for the spec-recoverable case).

Stdlib `ok` / `err` are reverted to their inline-C carrier bodies pending
the resolution above. `some` / `none` polymorphization is deferred for the
same reason. `pair` and `tcons-of` were already on `(make-struct ...)`
positional form and are unaffected.

## Related

- [docs/upcoming/m2b-make-struct-design.md](../upcoming/m2b-make-struct-design.md)
  — the design doc whose Phase 2 this report blocks.
- [make-struct-phantom-typeparam-lowering.md](make-struct-phantom-typeparam-lowering.md)
  — the prior report on the same class of unresolved-tyvar cast collapse.
  Phase 1's emit-side recovery patch fixes the monomorphized-clone case;
  the generic carrier emit (this report) is the remaining surface.
- The M2a inference path in `src/compiler/emit_fns.c:629-787` is the
  existing precedent for option (a) — it already knows how to synthesize a
  Result-shaped body from the StructDef when a payload arg is box-spilled.
