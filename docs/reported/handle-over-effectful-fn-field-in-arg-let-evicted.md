# A `handle` over an effectful fn-field call, inside a `let` in argument position, is refused

**Severity: medium.** A legal program is refused at build time. This is an
expressiveness gap, not a miscompile. Found 2026-10-01 by the type fuzzer: every
one of its `GEN_REJECT`s in an 800-case run (11 of 11) is this shape, through
the `fn_field_eff` crossing.

## Repro

```turmeric
(defeffect Ef [x :int] :int)
(defstruct FE :copy [run : (fn [int] int) #fx{Ef}])
(defn fe [v : int] : int (perform (Ef v)))
(defn main [] : int
  (println (let [s (make-struct FE fe)]
             (handle (.run s 3) (Ef [x] k) (resume k x))))
  0)
```

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset, and the direct emitter cannot lower `perform`.
```

`TUR_TRACE_EVICT=1` reports `SIG-TAINT` for both `fe` and `main`.

The same handle with the `let` OUTSIDE the call works and prints 3:
`(let [s (make-struct FE fe)] (println (handle (.run s 3) ...)))`. So does
`(handle (do (.run s 3) 0) ...)` under a let-bound `s`.
`tests/fixtures/fn-field-typed-float` covers the `:nil`-returning effects, which
work.

## Shapes that are refused

- `(println (let [s (make-struct FE fe)] (handle (.run s 3) ...)))`
- `(handle (.run (make-struct FE fe) 3) ...)` as an argument
- `(handle (let [s (make-struct FE fe)] (.run s 3)) ...)` as an argument

## Fix direction

Find which form `ensure_S`'s taint fixpoint (`emit_cps_ir.c`) marks
permanently tainted when the handled body sits under an argument-position
`let`. The working twin differs only in where the `let` is. Once fixed, the
fuzzer's `fn_field_eff` crossing generates no `GEN_REJECT`s, and its count
is the regression check.
