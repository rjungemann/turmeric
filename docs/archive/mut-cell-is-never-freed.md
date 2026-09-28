# The shared cell of a lambda-captured `^mut` is never freed

**RESOLVED 2026-09-28: fix direction 1, freed at the `let`'s scope end.**

The hidden binding that holds the cell is marked (`Binding.is_mut_cell`, set
in `elab_let_mut_to_cell`), and `emit_let_value` frees it after the body --
`tur_region_free` on the default build (the ctor allocates with
`tur_region_alloc_or_malloc`, so region memory is left to its generation),
plain `free` under `TUR_REGIONS=0` -- when `let_binding_mut_cell_freeable`
proves every closure capturing it is dead by then (emit_expr.c).

The proof leans on the one fact that makes the cell different from an
ordinary heap value: its pointer never appears in user code.  The `^mut`
name is an alias whose reads and writes elaborate as `(.v cell)` field
accesses, so the pointer leaves the `let` only inside the env of a closure
that captures it.  `mut_cell_escapes` walks the let's later inits and body:

- a `(.v cell)` read or write is not an escape;
- a closure that captures the cell must be a `let` init that
  `let_binding_env_freeable` already proves does not escape -- which is
  exactly where a lambda handed to a non-retaining parameter ends up (the
  `__borrowc` hoist) -- and no closure in ITS body may let the cell out in
  turn (the walk recurses into it);
- a capturing closure anywhere else (returned, stored, in a `letrec`) is an
  escape, and so is any node the walk does not know, via the ordinary escape
  walk, where every mention of the cell counts.

A wrong "escapes" leaks, as before; nothing else changes.  The closure env
drop glue never touches a `:heap` capture, so the cell has exactly one
release.  Early exits (a `return`/`throw` in the body) skip the trailing free,
as the env frees beside it do.

Pinned by `tests/fixtures/mut-cell-freed-at-scope-exit` under
`tests/run-leak-check.sh` (`requires.leak-check`): the repro, a float cell
fresh per loop iteration, and two let-bound closures sharing one cell.  The
escaping shapes -- a counter returned from its `let`, a closure pushed into a
global from inside a borrowed lambda -- keep their cells, verified under ASan
with `closure-captured-mut-shared` (8 cells now freed there, output
unchanged).  A Saffron closure crosses dynamic calls, which the walk cannot
see through, so a Saffron cell is still kept (no free, no change).

The original report follows.

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
