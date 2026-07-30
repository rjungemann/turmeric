---
status: open
severity: medium
discovered: 2026-07-30
area: codegen (Vec element carrier bridge, by-value struct elements)
---

# `Vec` of by-value struct elements: `tur check` passes, cc rejects the emitted C

## Summary

A `(Vec T)` whose element `T` is a plain (non-`:heap`) `defstruct` type-checks
fine, but reading an element back produces invalid C -- with or without the
documented `(:: (vec-get v i) T)` ascription idiom. The `:heap` variant of the
same struct works (with an incompatible-pointer warning). Found by the
`tests/type-fuzz-src.py` probe phase.

## Repro

    $ cat > /tmp/vb.tur <<'EOF'
    (defstruct FzB [a : int])
    (defn main [] : int
      (let [v (:: (vec-new) (Vec FzB))]
        (vec-push! v (FzB 31))
        (let [b (:: (vec-get v 0) FzB)]
          (println (.a b))))
      0)
    EOF
    $ ./build/tur check /tmp/vb.tur ; echo rc=$?
    rc=0
    $ ./build/tur run /tmp/vb.tur
    /tmp/tur-build/vb_tur.c: ... error: invalid initializer
         tur_adt_FzB b_1303 = __ps_158;
    tur: cc invocation failed (status 256)

Without the let-bound ascription, field access on the raw `(vec-get v 0)`
fails the same way as `request for member 'a' in something not a structure
or union`.

## Controls

| Variant | Result |
|---|---|
| element is `:heap` struct, same code | runs, prints 31 (with `-Wincompatible-pointer-types` warning) |
| element is `(Option int)` (parametric heap container) | runs -- pinned by `tests/fixtures/vec-push-heap-struct-element-carrier-cast` |
| element is scalar (`int`, `float` via `(:: ... :float)`) | runs |

## Root cause (direction)

`vec-push!`/`vec-get` traffic in the int64 element carrier. The
concrete->carrier bridge added for heap struct/container elements
(docs/archive/history/vec-push-heap-struct-element-not-carrier-cast.md) covers
heap pointers, but a by-value struct has no pointer-sized carrier form: the
push side spills or truncates, and the read side initializes the by-value
`tur_adt_FzB` straight from the `int64_t` slot -- the same lossy erasure round
trip as `result-monad-bind-typed-boundary-miscompiles`, at a different
boundary.

## Fix directions

1. Either bridge by-value struct elements through a boxed/spilled
   representation on push and un-spill on `vec-get`, or
2. Reject non-carrier-representable element types at `(Vec T)` formation with
   a real diagnostic ("by-value struct elements need `:heap`") -- a checker
   error beats invalid C.
3. Fixture either way; `tests/type-fuzz-src.py --known-probes` pins the shape
   until then (retire its `known_bug_slug` row when fixed).

## Workaround

Mark the element struct `:heap`.

## Guide upkeep

When this report is resolved -- or any representation/bridge it describes
changes shape on the way -- update
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md)
in the same PR: fix the representation inventory, move this report's row out
of the missing-cells table, and correct the link when the report moves to
`docs/archive/`.
