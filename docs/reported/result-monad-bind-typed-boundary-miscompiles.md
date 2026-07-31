---
status: open
severity: high
discovered: 2026-07-29
area: stdlib + compiler (HKT method result typing, stdlib/result.tur)
---

# `bind` / `do-m` over `Result` miscompiles across a typed `(Result A B)` boundary

## Investigation update (2026-07-31, consolidation increment 2)

The crash is now pinned one level deeper than the original root cause. On
current main the TYPED BOUNDARY itself is handled -- the emitted `g` spills
`(f n)` by value, passes its address to the carrier `bind`, and re-wraps the
int64 result into the by-value `(Result int int)` correctly. The surviving
defect is a **return-ABI mismatch in the continuation pairing**:

- the lambda's poly wrapper is emitted returning the by-value aggregate
  (`static tur_adt_Result__int__int __poly_N(void*, int64_t)`), because the
  elab-side `boxes_aggregate` gate (elab_typeclasses.c, the Gap-2 comment
  block) assumes "a CONCRETE receiver resolves to the instance's own
  by-value entry point" and skips the carrier-spill shim;
- but `Result`'s partially-applied instance head resolves to the CARRIER
  base entry (`__inst_Monad_bind_Result_tyvar`), which invokes the
  continuation through `(int64_t(*)(void*,int64_t))k.fn` -- a struct-return
  fn cast to an int64-returning pointer (RAX:RDX vs RAX), handing bind
  garbage that the (otherwise correct) boundary re-wrap then dereferences.

So the fix is not at the boundary: the wrapper/callee ABI pairing must be
decided by WHICH entry point the dispatch selects (by-value spec => raw
aggregate wrapper; carrier base => spill-shimmed wrapper via
`ensure_aggregate_spill_shim`), not by receiver abstractness. The
by-value-spec side must stay unshimmed -- that pairing is byte-for-byte
load-bearing for the working `Option` path (see the emit-side comment at
the EX_POLY_WRAP spill gate, emit_expr.c). The selection happens at emit
(abi specialization), after the elab-side gate has already chosen -- which
is the same split-decision anatomy as every other cell in this family, and
increment-4 material if the pairing cannot be decided in one place sooner.


## Summary

`Monad [(Result _ B)]` works at the carrier level but breaks the moment the
result flows through a typed `(Result A B)` boundary. Two distinct symptoms,
same cause:

- Returning the `bind` result from a `(defn ... : (Result A B))` and reading it
  with `ok?` / `ok-val` -> **segfault at runtime**.
- Ascribing the `bind` result with `(:: ... (Result int cstr))` -> **invalid C**
  (`error: invalid initializer`).

`Option` is unaffected -- `bind` and `do-m` over `(Option A)` round-trip
cleanly through typed functions. The divergence is what makes this worth
fixing rather than documenting: the two obvious monads in the stdlib behave
differently at the same call shape.

Found while reworking `docs/guides/effects-vs-monads.md`; the `do-m`-over-
`Result` example that would naturally sit next to the `Option` one cannot be
written.

## Repro

### Segfault

    $ cat > /tmp/r1.tur <<'EOF'
    (defn f [n : int] : (Result int int)
      (if (= n 0) (err 7) (ok n)))
    (defn g [n : int] : (Result int int)
      (bind (f n) (fn [x] (ok (* x 2)))))
    (defn main [] : int
      (let [r (g 5)] (println (if (ok? r) (ok-val r) -1)))
      0)
    EOF
    $ ./build/tur run /tmp/r1.tur
    Segmentation fault

Expected: `10`. Reproduces identically with `(Result int cstr)`, and with `do-m`
in place of the explicit `bind`.

### Invalid C

    $ cat > /tmp/r2.tur <<'EOF'
    (defn f [n : int] : (Result int int)
      (if (= n 0) (err 7) (ok n)))
    (defn main [] : int
      (let [r (:: (bind (f 5) (fn [x] (ok (* x 2)))) (Result int int))]
        (println (if (ok? r) (ok-val r) -1)))
      0)
    EOF
    $ ./build/tur run /tmp/r2.tur
    /tmp/tur-build/r2_tur.c: In function 'main':
    /tmp/tur-build/r2_tur.c:7003:47: error: invalid initializer
     7003 |             tur_adt_Result__int__int r_1311 = __ps_167;
    tur: cc invocation failed (status 256)

### What does work

The carrier-level path is fine, which localizes the fault to the boundary:

    (defn res-ok [r : int] : int
      ```c struct { bool is_ok; int64_t ok_val; int64_t err_val; } *p = (void*)(intptr_t)r;
           return p->is_ok ? p->ok_val : -1; ```)
    (defn main [] : int
      (println (res-ok (bind (:: (ok 20) (Result int int)) (fn [x] (ok (+ x 1))))))
      0)
    ;; => 21

And the `Option` equivalent of the failing case works:

    (defn f [n : int] : (Option int) (if (= n 0) (none) (some n)))
    (defn g [n : int] : (Option int) (bind (f n) (fn [x] (some (* x 2)))))
    ;; (unwrap-or (g 5) -1) => 10

## Root cause

A typeclass method result carries the `int64_t` erasure -- `bind` hands back the
carrier pointer, not the by-value `tur_adt_Result__A__B` struct. At a
`(Result A B)`-typed boundary the compiler believes it has the by-value struct:

- The invalid-C case is that belief made visible -- it initializes a
  `tur_adt_Result__int__int` from an `int64_t`.
- The segfault is the same mismatch surviving to runtime, where `ok?` / `ok-val`
  read a by-value struct out of what is actually a pointer.

`Option` escapes because its carrier and its by-value form coincide at the
places these examples touch. `Result` is a two-parameter constructor whose
instance head is partially applied (`(definstance Monad [(Result _ B)] ...)`,
`stdlib/result.tur:324`), and the by-value ADT lowering
(`tur_adt_Result__int__int`) is a real struct -- so the erasure round trip is
lossy in a way `Option`'s is not.

Existing coverage misses this: `tests/fixtures/hkt-stdlib-option-result-
instances` exercises `Result` only through `Bifunctor` with carrier-level
inline-C extractors, never `Monad` through a typed boundary. Similarly
`tests/fixtures/hkt-stdlib-parser-instances` sidesteps it by declaring
`(defn two-sum [] : int (do-m ...))` -- returning `:int`, the erasure, rather
than `(Parser int)`.

## Fix directions

1. Decide what a typeclass method returns at a type-applied head. If the method
   result is to stay erased, the boundary needs an explicit re-wrap (the
   erasure -> by-value conversion) rather than a reinterpretation; if it is to
   be typed, the instance dispatch has to carry the element/error types through.
2. Whichever way, the `Option` and `Result` paths should agree -- the current
   split is the surprising part.
3. Fixtures: add `Monad [(Result _ B)]` through a typed `(Result A B)` function
   boundary, and the ascription form, both of which are absent today. A
   `(defn ... : (Parser int))`-returning `do-m` fixture would pin the same
   boundary for `Parser`.

## Workaround

Thread `Result` explicitly with `ok?` / `ok-val` / `err-val`, or use the
`Throw`-shaped effect formulation. `docs/guides/effects-vs-monads.md` notes the
defect and points at both.

## Guide upkeep

When this report is resolved -- or any representation/bridge it describes
changes shape on the way -- update
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md)
in the same PR: fix the representation inventory, move this report's row out
of the missing-cells table, and correct the link when the report moves to
`docs/archive/`.
