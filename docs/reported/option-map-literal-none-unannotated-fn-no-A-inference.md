---
title: `(option-map (none) (fn [x] ...))` -- literal-none + unannotated-fn leaves
  the element type `A` under-determined, producing a hard cc error
category: Elaboration / type inference -- Option none-as-NULL retirement (Track A)
severity: Low (niche ergonomics regression, NOT a silent miscompile). Mapping a
  *literal* `(none)` with an *unannotated* lambda fails to compile (hard cc error)
  after `option-map` was retyped from the carrier `:int` ABI to a pure-Turmeric
  by-value `(Option A) -> (Option B)` body. Every realistic use compiles and runs
  correctly; the workaround is a one-token annotation. Loud, not silent.
status: OPEN 2026-06-18. Surfaced while retyping stdlib `option-map` (bucket C,
  docs/reported/option-consumer-retype-byvalue.md step 3). Left as a documented
  limitation; the fix is an elab-side inference improvement (bind the generic's
  tyvar from a `^fat (fn [A] B)` closure arg when the by-value `(Option A)` arg
  is a carrier `#{Construct}` that carries no `A`).
---

# `(option-map (none) <unannotated-lambda>)` under-determines the element type

## Summary

After `option-map` was retyped to a pure-Turmeric by-value body

```turmeric
(defn option-map [A B] [o : (Option A) ^fat f : (fn [A] B)] : (Option B)
  (if (.is-some o) (some (f (.value o))) (none)))
```

a call that passes **both** a literal carrier `(none)` as `o` **and** an
**unannotated** lambda as `f` fails to compile:

```turmeric
(option-map (none) (fn [x] (* x 3)))   ; hard cc error
```

```
error: request for member 'is_some' in something not a structure or union
    if ((o).is_some) { ...
```

The same call with an annotated lambda compiles and runs fine:

```turmeric
(option-map (none) (fn [x : int] : int (* x 3)))   ; OK -> none
```

as does mapping over any *typed* Option value or a `(some ...)` literal:

```turmeric
(let [o (some 5)] (option-map o (fn [x] (* x 3))))  ; OK -> some 15
(option-map (some 7) (fn [x] (* x 2)))              ; OK -> some 14
```

## Root cause

The pure-Turmeric body reads `.is-some` / `.value` by value, so every
monomorphized spec must receive `o` as the concrete by-value `Option__A`
struct -- there is no valid carrier base (an `option_hymap(int64_t o, ...)`
base could not run `(o).is-some`, so the base is suppressed and only by-value
specs are emitted).

A by-value spec therefore needs the element type `A` resolved. `A` is normally
bound from `o`'s type:

- `(some 5)` / a `(some-typed-var)` -> `o : (Option int)` -> `A = int`.
- a literal `(none)` stays on the int64 carrier (carrier `0`), so its arg type
  is `int64_t` and contributes **no** `A`.

When `o` contributes no `A`, the only other source is the closure `f`'s
`(fn [A] B)` type. With an **annotated** lambda `f`'s type is `(fn [int] int)`
at the call, so `A = int` is collected (elab_call.c, the
`call_collect_type_bindings(expected_full, args[i]->type, ...)` arm). With an
**unannotated** lambda, `A` is not collected from `f` at binding-collection
time, so the spec's `o` slot lowers to the carrier `int64_t` while the body
accesses it by value -- the inconsistent spec the cc rejects.

## Minimal repro

```turmeric
(defn main [] : int
  (println (unwrap-or (option-map (none) (fn [x] (* x 3))) 99))  ; cc error
  0)
```

Observed: cc error (`request for member 'is_some' ...`).
Expected: prints `99` (none maps to none; default returned).

## Proposed fix directions

1. **Elab (preferred):** when collecting a generic call's type bindings, also
   bind tyvars that appear in a `^fat (fn [A] B)` closure parameter from the
   closure arg's resolved type, so `(fn [x] (* x 3))` (resolved to
   `(fn [int] int)` via `*`) yields `A = int` even when the `(Option A)` arg
   is a carrier `(none)`. This closes the gap for the whole class of by-value
   generic consumers, not just `option-map`.
2. **Emit (fallback diagnostic):** detect a spec whose by-value-aggregate
   param lowered to the int64 carrier (param declared `(Option A)` but
   `arg_type` c-names to `int64_t`) while the body accesses it by value, and
   raise a clean elaboration error ("element type of `option-map` is
   under-determined; annotate the function or the option") instead of leaking
   a cc error.

## Validation

`tests/fixtures/option-construct-byvalue-return-spec/` exercises the working
by-value `(some ...)`/`(none)` return-position paths. A fix should add a
fixture for `(option-map (none) (fn [x] (* x 3)))` printing `99`.

## Related

- [docs/reported/option-consumer-retype-byvalue.md](option-consumer-retype-byvalue.md)
  -- the retype that surfaced this (bucket C, step 3).
- [docs/reported/option-none-as-null-byvalue-param-segfault.md](option-none-as-null-byvalue-param-segfault.md)
  -- the by-value-param carrier bridge (handles `(none)` -> by-value param when
  `A` is known; this report is the residual when `A` is *not* known).
