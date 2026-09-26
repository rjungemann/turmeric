# The shared cell of a lambda-captured `^mut` is never freed

**Severity: low.** One small allocation (8 bytes, 16 for an `any`) per
evaluation of a `let` whose `^mut` a lambda captures. No wrong answer. Filed
2026-09-26 with the fix for
[compiled-closure-copies-a-captured-mut](../archive/compiled-closure-copies-a-captured-mut.md),
which introduced the cell.

## Repro

```turmeric
(defn apply3 [f : (fn [int] nil)] : nil (f 1) (f 2) (f 3))
(defn main [] : int
  (let [^mut acc 0]
    (apply3 (fn [x : int] : nil (set! acc (+ acc x))))
    (println acc))
  0)
```

Built with `tests/run-leak-check.sh`'s flags (`-fsanitize=address`, the
program run with `detect_leaks=1`): `8 byte(s) leaked in 1 allocation(s)`,
allocated in `ctor_TurMutCell_TurMutCell`. In a loop, one per iteration.

## Why

The cell is a `:heap` struct (`TurMutCell`, stdlib/pair.tur), allocated with
`tur_region_alloc_or_malloc` like every `:heap` constructor, and nothing owns
it: the closures hold the pointer, and `:heap` values are not reference
counted. Inside a `with-region` bracket the rewind reclaims it; everywhere
else it lives forever -- the same model as the Scheme lowering's `R7rsBox`.

## Fix directions

1. **Free at the `let`'s scope end when no capturing closure escapes.** The
   direct emitter already decides, per closure, whether its env is freed at
   the binding's scope end (`let_binding_env_freeable`) or after a
   non-retaining call (the argument-hoist drain). The cell is freeable exactly
   when every closure that captures it is one of those. The common shape --
   the report's repro, a lambda handed to a non-retaining callee -- is.
2. **Or make the cell reference counted.** Model R (closure-drop-glue) already
   retains an `rc` capture at the env fill and releases it in the env's drop
   glue, so an `rc` cell would be freed with its last closure. Blocked today:
   `(set! (.v c) x)` through an `rc` of a PARAMETRIC struct is refused
   ("receiver must be a struct or rc<Struct>, got rc<(type-app ? ?)>"); a
   non-parametric cell works, so per-element-type cells are a workaround.
