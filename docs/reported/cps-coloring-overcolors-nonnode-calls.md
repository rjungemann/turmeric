# CPS coloring over-colors any function that calls a non-node callee (constructors, stdlib)

**Severity:** medium -- a **coverage** limitation for the CPS-IR-to-C backend,
not a miscompile. The over-coloring itself is safe (coloring more functions never
produces wrong code); the cost is that needlessly-colored functions cannot be
delegated as cps->direct calls, so a whole class of colored code falls back.
Gated behind `--enable=cps-backend`.

## Summary

The may-capture coloring in `src/passes/cps.c` marks a function colored if it
makes an **unresolved** call -- `cps_collect_calls` sets `has_indirect = true`
(cps.c:340) whenever an `EX_CALL`'s `fn_binding` is not one of the top-level
function nodes. That set does **not** include auto-generated constructors,
stdlib functions, or externs. So *any* function whose body calls one of those is
conservatively colored -- including a pure `make-struct` helper, whose body is a
call to the (non-node) constructor.

With the CPS backend this over-coloring is what blocks Option/Result/record
construction via helper functions across an effect boundary: a helper like `some`
or a user `mkwid` is colored, so a **non-tail** call to it (`(let [o (some v)]
...)`) needs a heap-reified join -- the pre-existing C1/C3 `needs_heap_join`
limitation -- which evicts the caller and forces the whole function back to the
fiber path. An *inline* `make-struct` works (it lowers to a constructor call the
CPS backend delegates directly), but the far more common helper-call form does
not.

## Minimal repro

```turmeric
;; pure make-struct helper -- no control op anywhere
(defstruct Wid [A] (v A))
(defn mkwid [x : int] : (Wid int) (make-struct Wid :v x))
```

`tur emit-c ... --enable=cps-backend --dump-cps` shows `cps-fn mkwid` (i.e.
`mkwid` is colored). Used non-tail across an effect it evicts the caller:

```turmeric
(defeffect E [] :int)
(defn f [] : int
  (let [v (perform (E))]
    (let [w (mkwid v)]      ; non-tail cps->cps to a (needlessly) colored fn
      (.v w))))
(defn run [] : int (handle (f) (E [] k) (resume k 42)))
```

`f` falls back (no `f__cps`), even though `mkwid` reaches no control op. The
same happens for stdlib `some` / `none` / `ok` / `err` and map/list builders,
because they too are `make-struct` helpers.

## Root cause

Two composing causes:

1. **Coloring over-approximation** (`src/passes/cps.c:333-341`,
   `cps_collect_calls`). An `EX_CALL` to a binding not found by `cps_find_node`
   (constructors, stdlib, externs) sets `has_indirect = true` -> the function is
   colored. This is correct-but-coarse: an unresolved call *might* reach a
   control op, but a constructor / extern-c / known-pure callee never does.
2. **Non-tail cps->cps has no heap join** (`needs_heap_join`,
   `src/compiler/emit_cps_ir.c`). Even once a function is (correctly or not)
   colored, calling it in non-tail position requires reifying the join
   continuation onto the heap chain, which the backend does not yet do -- so the
   caller is evicted. This is the documented C1/C3 subset limitation.

Either fix alone unblocks the common case; both are worth doing.

## Fix directions

1. **Coloring precision.** Do not set `has_indirect` for a call whose callee is
   resolved-but-non-node and known not to reach a control op -- constructors and
   `extern-c` functions in particular (they have no Turmeric body / no effects).
   A cheap version: recognize constructor call targets (and externs) and treat
   them as non-coloring edges rather than indirect. This un-colors the large
   population of pure `make-struct` helpers (Option/Result/map builders).
2. **Non-tail cps->cps heap join.** Implement the heap-reified join for a
   non-tail call to a colored, CPS-emitted callee (lift the continuation after
   the call onto the DK chain and resume it), removing the `needs_heap_join`
   eviction. This is the more general fix and also helps non-struct colored code.

Landing (1) is the smaller step and directly unblocks Option/Result/record
helpers; (2) is the broader capability.

## Related

- N3 of the non-scalar plan
  (`docs/upcoming/v1/cps-backend-non-scalar-values-plan.md`): the struct/ADT
  *local* slice landed there works for inline `make-struct`; this report is the
  "still open" helper-call case noted in that section.
- `needs_heap_join` in `src/compiler/emit_cps_ir.c` (the C1/C3 non-tail
  cps->cps limitation).
- `docs/reported/cps-coloring-ascription-hides-control-op.md` -- a different
  coloring-vs-backend precision gap (that one *under*-colors; this one
  *over*-colors).
