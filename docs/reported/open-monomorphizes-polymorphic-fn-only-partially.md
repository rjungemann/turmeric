---
title: Codegen does not monomorphize polymorphic stdlib helpers called from inside an `open` body
category: Codegen / monomorphization gap
severity: Latent expressiveness hole. Type-checking of `(sized-buf-free buf)` inside an `(open packed [n buf] ...)` body succeeds, but the C-codegen monomorphizer does not emit the `sized_hybuf_hyfree` instantiation, so the resulting C file references an undeclared function. The accept fixture works around it by carrying `requires.no-leak-check` and omitting the free call from the body. Surfaces immediately downstream of the `elab_open` fix in [pack-open-phantom-opaque-body-type-collapses.md](pack-open-phantom-opaque-body-type-collapses.md).
description: After the open-projects-applied-form fix, `buf` is bound at `(SizedBuf <skolem>)` inside the body. The elaborator accepts a call to a polymorphic helper like `(defn sized-buf-free [n] [b : (SizedBuf n)] : nil ...)`, but the call's `n` is bound to the open's abstract skolem rather than to a concrete `(Static k)`, so the monomorphizer's specialization table does not produce a concrete instantiation. Other call sites (e.g. `sized-buf-len`) are emitted because some sibling file already monomorphized them at concrete sizes; helpers that are only reachable through an open get skipped.
status: OPEN. Workaround in `tests/fixtures/sized-buf-existential-pack-open` (no-leak-check + omit free). Closing this gap is the second of two follow-ups for full SizedBuf round-tripping through existential lift.
---

# `open` body sees polymorphic helpers as monomorphizable but codegen drops them

## Symptom

```turmeric
(load "stdlib/sized.tur")
(load "stdlib/sized-buf.tur")

(defn mk5 [] : (SizedBuf int)
  (:: (sized-buf-new-zeroed 5) :SizedBuf))

(defn main [] : int
  (let [packed (pack (mk5) (exists [n] (SizedBuf n)))]
    (open packed [n buf]
      (println (sized-buf-len buf))
      (sized-buf-free buf)))      ; <- type-checks; codegen drops the instantiation
  0)
```

Compiled with `-Xsized-types`:

```
error: call to undeclared function 'sized_hybuf_hyfree'
note: did you mean 'sized_hybuf_hylen'?
```

`sized_hybuf_hylen` is emitted because some other call site (or the
prelude) already exercises it at a concrete size; `sized_hybuf_hyfree`
is only reached through the body of `open` here and so falls off the
specialization worklist.

## Workaround

`tests/fixtures/sized-buf-existential-pack-open` omits the free call
from the body and carries `requires.no-leak-check` so ASan does not
flag the SizedBuf leak. This is acceptable for documenting the type-
checking fix but is the wrong long-term shape: a user who naturally
wants to drop a linear handle inside an open body cannot.

## Root-cause hypothesis

The monomorphization worklist seeds from "concrete call sites" --
calls where every type parameter is bound to a concrete Type. Inside
an open body the call site has its polymorphic arg bound to a free
skolem (`TY_STRUCT` with `def=NULL`), which the seeder treats as "not
ready to monomorphize" and silently drops instead of emitting a generic
specialization or queuing a deferred instantiation.

A correct fix probably emits a single skolem-erased instantiation
(the runtime carrier is `:int` for every `(SizedBuf <anything>)`), or
treats `def=NULL` skolems the same as any concrete carrier-equivalent
type at codegen seed time.

File pointers to validate against during a fix:
- `src/compiler/emit_*.c` -- look for the monomorphization seed loop.
- Anywhere that filters calls by "all tyvars bound to concrete Type".

## Validation

- Add an accept fixture (or extend the existing
  `sized-buf-existential-pack-open`) that includes a `sized-buf-free`
  call inside the open body, and remove the `requires.no-leak-check`.
- Run the full suite; expect 1552+1 = 1553 pass / 82 fail (no
  regressions on the in-flight baseline).

## Related

- `docs/reported/pack-open-phantom-opaque-body-type-collapses.md` --
  parent fix (type-checking half). This report covers the codegen
  half that surfaced as a side effect.
